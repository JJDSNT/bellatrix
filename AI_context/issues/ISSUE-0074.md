---
id: ISSUE-0074
title: "Chipset on its own core: the accelerated-Amiga shape"
status: investigating
priority: high
type: research
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-30
tags:
  - rigel
  - chipset
  - scheduling
  - multicore
blockers: []
related_files:
  - src/amiga/bus.c
  - AI_context/issues/ISSUE-0068.md
---

# The question this answers

Measured on a Pi 3 (ISSUE-0068): the chipset runs at 112% of realtime, and
because Bellatrix drives chipset time from modelled 68000 cycles at two per
colour clock, that caps the guest CPU at 7.99 MHz-equivalent. **A cycle-exact
chipset driven by modelled CPU cycles throttles the machine to the speed of the
machine it emulates.**

Three answers were identified. This is the first.

# The shape

```text
core N          core M
  CPU     <-->  chipset
free-running    realtime
```

The CPU free-runs on one core at JIT speed. The chipset runs on another at its
own rate, which the same measurement says it can sustain. Neither waits for the
other except at the points where they genuinely interact: an MMIO transaction,
an interrupt, a DMA cycle.

That is a physically accurate model of what an accelerated Amiga *is*: a fast
CPU on a board, and a chipset that keeps running at 7.09 MHz whatever the CPU
does. It is the PiStorm arrangement.

# It has been built here before

The `legacy` branch ran exactly this: `src/runtime/core_chipset.c`, with Core 2
owning the chipset and Core 3 the IO, described in
`bellatrix-phasing-and-logs-principle` as determined phases -- launcher on Core
0, runtime multicore. It was left behind when the tree was reset, and recorded
since as multicore scaffolding rather than a destination.

**This measurement is the argument for it.** It was set aside as complexity
without a demonstrated need; the need is now demonstrated.

# Why it moved to the front

It was written as "second, because concurrency is a cost". Two things since:

**Decoupling on one core cannot get there.** ISSUE-0075 is implemented and the
arithmetic bounds it: the chipset costs 250 ns per colour clock and realtime
wants one every 282, so running it at realtime takes 88.7% of a core and leaves
11.3% for the CPU. Decoupling stops the CPU being the pacer; it does not create
CPU time.

**Three quarters of the machine is switched off.** From Emu68's
`secondary_boot()`:

```c
#ifdef PISTORM_ANY_MODEL
    if (cpu_id == 1) { if (async_log) serial_writer(); }
    else if (cpu_id == 2) { ps_housekeeper(); }
#else
    (void)async_log;
#endif

    while(1) { __asm__ volatile("wfe"); }
```

`PISTORM_ANY_MODEL` is not defined in our build and no patch of ours touches
this, so cores 1, 2 and 3 enable their caches, print `Started CPUn` and park
forever. The Pi log shows exactly that.

So this is not adding concurrency to a busy machine. It is using hardware that
is currently idle, and the alternative is leaving 75% of the CPU unused to
avoid a cost. Note also which core upstream itself puts its housekeeper on.

# What would have to be true

- Rigel is not thread-safe in any documented way; the boundary would need one.
- The chip RAM callbacks (`machine_chip_ram_read16/write16`) would be called
  from the chipset core while the CPU core writes the same memory.
- `amiga_irq_sync()` publishes into `__m68k_state->INTF.IPL`, which the CPU core
  reads -- already an atomic store, which is the easy half.
- Emu68's core assignment: `bellatrix-core0-target-is-control` records that
  CPU-on-Core-0 is temporary stabilisation scaffolding, not the baseline, so the
  placement question is open rather than settled.


# What the legacy branch already solved

`legacy:src/runtime/core_chipset.c` is 951 lines of exactly this, and its value
is not the code -- our clock is different now -- but four mechanisms with the
failure that produced each written beside them.

**1. The lock, and why release is conditional.**

```c
void core_chipset_lock_release(void)
{
    atomic_flag_clear_explicit(&s_chipset_access_lock, memory_order_release);
    /* Most releases are chipset finishing an uncontended Rigel step. The old
     * unconditional `dsb sy; sev` broadcast woke every PE and converted work
     * completions into empty host-loop iterations. A release store is the
     * lock's ordering contract; only publish an event when a waiter actually
     * parked in WFE. */
    if (atomic_load_explicit(&s_chipset_lock_waiters, memory_order_relaxed) != 0u)
        asm volatile("dmb ishst\n\tsev" ::: "memory");
}
```

**2. The beam snapshot, and what it cost not to have one.** A seqlock over
`vpos`, `hpos`, `line_clocks`, `frame_lines`, `vposr_high`, `lof`, `lol`,
published by the chipset and read lock-free by the CPU. Without it every VHPOSR
poll was a critical-MMIO rendezvous that had to flush and wait for the chipset
core: the recorded number is `caught_up` going from 11k to 783k. **A guest
polling the beam is the normal case, not an edge case.**

**3. Posted writes.** Non-critical custom-register writes queued with the CPU's
emulated time and applied by the chipset when it reaches that stamp -- no lock
and no rendezvous on the CPU side, and better temporal fidelity than applying
them inline at the chipset's stale time. The PiStorm `wb_push`/`wb_task`
pattern.

**4. A bound on divergence.** `CHIPSET_MAX_BACKLOG_CCK 8192`, with the failure
it exists for: "without this bound the target diverges without limit (observed:
chipset >400M CCK behind), making the emulated machine's sense of time
meaningless."

## What changes because of ISSUE-0075

Legacy paced the chipset from the CPU: the CPU published `s_cpu_cck_target` in
modelled cycles and blocked when it ran more than 8192 CCK ahead. **We no
longer need that half, and it is the half that throttles.** With chipset time
taken from `CNTPCT_EL0`, the chipset core paces itself against real time, the
CPU never publishes a target and never blocks, and mechanism 4 becomes
unnecessary -- there is no target to diverge from.

Mechanisms 1, 2 and 3 stay necessary and are the work.

The combination is the point: **CPU at full JIT speed on one core, chipset at
true realtime on another.** That is what the machine was built to be.

# The first step

Emu68's secondary cores park in `secondary_boot()`. Before any of the above can
be written, one of them has to run our code at all -- a patch giving core 2 a
Bellatrix entry point under `CONFIG_RIGEL`, and a serial line from it proving
it arrived. Everything else builds on a core that is demonstrably ours.
