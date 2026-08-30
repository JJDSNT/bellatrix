---
id: ISSUE-0074
title: "Chipset on its own core: the accelerated-Amiga shape"
status: backlog
priority: medium
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

# Why it is not the first thing to try

Chosen second, deliberately, and ISSUE-0075 goes first. Two cores sharing a
chipset is a concurrency problem -- every MMIO transaction becomes
cross-core, the IPL path becomes cross-core, and the memory Rigel reads is
memory the CPU writes. The legacy notes carry the scars: a race between the
multicore and single-core IPL delivery paths is recorded as "a fragilidade
arquitectural exposta" in `bellatrix-vertb-idle-loop-2026-07-16`.

If decoupling on one core is enough, none of that has to be paid for.

# What would have to be true

- Rigel is not thread-safe in any documented way; the boundary would need one.
- The chip RAM callbacks (`machine_chip_ram_read16/write16`) would be called
  from the chipset core while the CPU core writes the same memory.
- `amiga_irq_sync()` publishes into `__m68k_state->INTF.IPL`, which the CPU core
  reads -- already an atomic store, which is the easy half.
- Emu68's core assignment: `bellatrix-core0-target-is-control` records that
  CPU-on-Core-0 is temporary stabilisation scaffolding, not the baseline, so the
  placement question is open rather than settled.
