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


# 2026-08-30: the core is ours

Patch `emu68/0020` hands core 2 to Bellatrix from `secondary_boot()`, and
`src/amiga/core.c` receives it:

```text
[BOOT] Started CPU2
[BELLATRIX:RIGEL] enabled; address decode owned by Rigel
[BELLATRIX:RIGEL:CORE] core 2 is the chipset's
```

Core 2 because that is where upstream puts its own housekeeper on PiStorm
builds, so the placement is not a new claim about which core is free.

## The ordering trap, met immediately

The first version tested the flag on arrival and returned if it was clear. It
never fired, and the log says why: `Started CPU2` is the thirty-third line of
the boot and `amiga_bus_init()` runs twenty-five lines later. **The secondary
core reaches its entry point during Emu68's boot, before Bellatrix exists.**

So the core waits to be wanted instead:

```c
while (!core_wanted)
    __asm__ volatile("wfe" ::: "memory");
```

which is the same parking Emu68 would have done, so a machine that never wants
the core is no worse off -- it simply never gets the SEV. Worth writing down
because the comment warning about exactly this was already in the file when the
bug was written.

## What this is and is not

It is a core that has been shown to arrive, which is what everything above it
has to be written against. **The chipset has not moved yet.** Next, in order:

1. the lock, with the conditional release legacy learned the hard way;
2. the chipset loop itself on this core, paced by `CNTPCT_EL0` rather than by
   published CPU cycles -- that is what makes the CPU core free rather than
   merely less blocked;
3. the seqlock beam snapshot, before any guest polls VHPOSR across cores;
4. posted writes, if the rendezvous cost shows up where legacy says it will.


# 2026-08-30: the chipset runs there

Steps 1 and 2 of the list above.

**The lock** is legacy's, including the part that is not obvious:

```c
void amiga_core_lock_release(void)
{
    atomic_flag_clear_explicit(&chipset_lock, memory_order_release);
    if (atomic_load_explicit(&chipset_lock_waiters, memory_order_relaxed) != 0u)
        __asm__ volatile("dmb ishst\n\tsev" ::: "memory");
}
```

Almost every release is the chipset core finishing an uncontended step, and an
unconditional broadcast wakes every PE to no purpose.

**The loop** is `amiga_clock_run_on_core()` in `src/amiga/bus.c`: acquire,
advance against `CNTPCT_EL0`, drain, release, forever. It spins rather than
sleeping, because this core exists to keep realtime and nothing could
usefully wake it that is not the passage of time. Before the chipset is armed
it idles, which is the same laziness the single-core path has.

## Three things got simpler, not harder

- **The flush before MMIO is gone.** It existed so a register read would not
  see a chipset frozen since the CPU last happened to step it. A chipset that
  is always current does not need flushing, and the CPU no longer steps it at
  all.
- **`publish_cpu_progress` returns immediately.** The CPU stopped being the
  chipset's clock.
- **`CHIPSET_ACTIVE` stays clear**, so `EMIT_STOP` parks the CPU in WFE again
  rather than yielding to `MainLoop` to keep chipset time. The chipset core
  raises the IPL and sends the event, which is what wakes it -- and is what a
  real machine does.

## What QEMU can and cannot say

```text
[BELLATRIX:RIGEL:CORE] core 2 is the chipset's
[BELLATRIX:RIGEL] clock armed by a write to $00bfed01
[BELLATRIX:RIGEL:CORE] chipset running here now
[BELLATRIX:RIGEL:PERF] 76003913 CCK in 94040 ms ... 22% of realtime, 226 CCK/call
```

Verified: the core is ours, the chipset runs there, the lock does not deadlock
across a boot, and the boot reaches further in the same wall time than either
single-core mode did -- 181 lines against 141.

**Not verified, and QEMU cannot**: whether the CPU is now free. QEMU emulates
four cores onto host threads that compete for the same machine, so a second
core costs rather than pays. The number this was built for is a hardware
number.

## Still to do

3. the seqlock beam snapshot, before any guest polls VHPOSR across cores --
   legacy's `caught_up` went 11k to 783k without it;
4. posted writes, if the rendezvous cost shows up where legacy says it will.

Both are optimisations of a path that now works. Neither should be written
before a hardware measurement says the path is worth optimising.
