---
id: ISSUE-0075
title: "Decouple chipset time from CPU time: let the CPU run free"
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
  - timing
blockers: []
related_files:
  - src/amiga/bus.c
  - AI_context/issues/ISSUE-0068.md
---

# The question this answers

Measured on a Pi 3 (ISSUE-0068): the chipset sustains 112% of realtime, and
Bellatrix drives chipset time from modelled 68000 cycles at two per colour
clock -- so the guest CPU is capped at 7.99 MHz-equivalent and AROS takes
thirteen minutes to boot. The chipset is fast enough; the coupling is what
costs.

**Decided 2026-08-30: try this one first.** The CPU should run free and the
chipset should be synchronised when the chipset needs it, not when the CPU
produces cycles.

# What "free" means precisely

The choice is not between coupled and uncoupled, but about **what chipset time
is a function of**, and there are two readings. Only one of them helps:

**Wall clock.** The chipset advances by real elapsed nanoseconds. The CPU
free-runs at JIT speed and never waits for it. This is what a real accelerated
Amiga is: a fast CPU on a board beside a chipset that keeps running at 7.09 MHz
whatever the CPU does.

**Deferred CPU cycles.** Still two cycles per colour clock, but caught up
lazily. This does not help at all -- the same total work is done, in bursts
rather than smoothly, and the machine stays throttled. Worth naming because it
is the reading that sounds like "lazy" and is not what is meant.

So: **chipset time becomes a function of wall-clock time.**

# What this buys and what it costs

**Buys.** The CPU runs at JIT speed. A guest waiting for VBLANK gets one every
20 ms of real time, which is more correct in real terms than what it gets
today. Sound plays at the right pitch. The machine stops being throttled to
7 MHz.

**Costs, and they are the honest part.** The ratio between CPU speed and
chipset speed stops matching a stock Amiga. Software calibrated by counting CPU
instructions against the beam -- a busy-wait tuned for 7 MHz, a copper effect
synchronised by loop count rather than by the beam registers -- behaves as it
would on an accelerated Amiga, which is to say sometimes wrongly. **That is not
a regression introduced here; it is the defining property of every accelerator
ever sold for the machine**, and it is why `SetPatch`-era software has degrade
switches.

It also means Rigel's cycle-exactness stops describing the *whole machine*. It
still describes the chipset exactly, which is what Rigel is for; what changes
is that the CPU is no longer in the same timebase. Anything measuring the two
against each other -- `tools/tests/timing/`, the Copperline oracle -- has to
fix the mode on both sides, the way `BELLATRIX_FRAME_POINTERS` has to be fixed
on both sides of a performance comparison.

# Therefore it is a mode, not a replacement

The CPU-driven path is what makes the guest's view of time match a stock
machine, and that is the right answer for compatibility testing even though it
is the wrong answer for a desktop. Both stay reachable.

# Design

- Chipset time accumulates from `CNTPCT_EL0` deltas rather than from published
  CPU cycles. The counter is already read for the `PERF` report.
- The existing machinery stays: the arming rule, the bounded quantum, the flush
  before MMIO, the STOP path. Only the source of `pending_cck` changes.
- **The catch-up has to be capped.** A long JIT translation pause, or a serial
  write, must not be followed by the chipset trying to replay a second of time
  in one burst -- that would reintroduce exactly the stall this removes, and it
  is the failure the legacy notes call an "expensive catch-up burst".
- The STOP path already advances the chipset while the CPU is idle, and under
  wall-clock time it becomes simpler rather than harder: idle time is real time
  and needs no synthetic cycle credit.

# How it will be known to work

The same two numbers, on the same hardware:

- the `PERF` line should still report near 100% of realtime, because the
  chipset's own cost has not changed;
- the boot should stop taking thirteen minutes, because the CPU is no longer
  waiting for it. The SD card's `12 KB/s` -- 97.5% of it spent outside the
  driver -- is the specific thing that should move.


# 2026-08-30: implemented, and the arithmetic that decides how far it goes

`src/amiga/bus.c` now takes chipset time from `CNTPCT_EL0` deltas when
`clock_wall_driven` is set, which it is by default. The existing machinery is
unchanged -- the arming rule, the bounded quantum, the flush before MMIO -- and
only the source of `pending_cck` moved. The catch-up is capped at about one
frame, so a pause drops chipset time rather than replaying it in a burst. The
STOP path needed nothing: it yields to `MainLoop`, which calls the publish
hook, and a stopped CPU does not stop real time.

## What it can and cannot buy, on one core

The Pi measurement decides this before any test does:

```text
chipset:  250 ns per colour clock  (measured)
realtime: 282 ns per colour clock

running the chipset at realtime costs 88.7% of a core
leaving 11.3% for the CPU
```

**Decoupling does not create CPU time. It stops the CPU being the pacer.** The
chipset still costs what it costs, and on one core whatever it does not spend
is all the CPU can have.

So the honest expectation for this mode on a Pi 3:

- the chipset keeps **true realtime**, which the coupled mode never did -- a
  VBLANK every 20 ms of real time, sound at the right pitch;
- the CPU gets about **an ninth of a core**, running the JIT, against the
  8 MHz-equivalent the coupled mode gave it. Better, and not by the order of
  magnitude the JIT is capable of.

## What that means for the choice

This is the argument for ISSUE-0074 arriving sooner than "second". A fast CPU
*and* a realtime chipset needs two cores' worth of time, because one core does
not have it. Nothing about that is a defect in Rigel -- 250 ns per colour clock
on an A53 is 7.1x a modern x86 and entirely reasonable -- it is arithmetic.

Worth testing on hardware anyway, and cheaply, for two reasons. The chipset
running at true realtime is correctness rather than speed, and it is worth
having on its own. And the arithmetic above predicts a ratio, not a boot time:
whether an ninth of a Pi 3 core running Emu68's JIT is a usable machine is a
question a measurement answers better than a division does.
