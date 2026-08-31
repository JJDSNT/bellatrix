---
id: ISSUE-0075
title: "Decouple chipset time from CPU time: let the CPU run free"
status: investigating
priority: high
type: research
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-31
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


# 2026-08-31: wall-clock time made QEMU unusable, and the knob that gives it back

The section "Therefore it is a mode, not a replacement" above no longer
describes the tree. `50ede60` removed the CPU-driven path outright, together
with `patches/emu68/0017` and `0019`, and real time is now the only source of
chipset time. The reasoning that removed it stands. What was not noticed is
that the CPU-driven clock had a second property nobody was buying it for: it
was **a function of the guest**, so it was invariant to how fast the host ran.
A slow host produced a slow chipset and a slow CPU together, in proportion, and
the machine stayed coherent. Wall-clock time has no such invariance, and QEMU
is the host where that matters.

## What a host that cannot deliver real time actually gets

Not a slower chipset. Measured on an idle 8-core x86 host, QEMU 8.2.2,
`-M raspi3b -accel tcg,tb-size=64` (MTTCG is on by default there -- `info cpus`
reports four distinct `thread_id`s -- and `-smp` cannot be raised, the machine
is fixed at four CPUs, so none of this is a host-core shortage):

```text
[BELLATRIX:RIGEL:PERF] 76003459 CCK in 110193 ms over 358142 calls
                    -> 1449 ns/CCK, 689729 CCK/s (19% of realtime), 212 CCK/call
```

at 115.719 s of wall clock. Read against the wall clock rather than against
itself, that line says:

- **95.2%** of the chipset core's wall time is spent inside `amiga_clock_step`
  (110.193 s of 115.719 s). The core is saturated; it never idles.
- real time asked for 410.4M colour clocks and 76.0M were delivered, so
  **81.5% of them were discarded** by `AMIGA_CCK_MAX_CATCHUP`. The mode is
  already running at a fifth of real time -- it is simply doing so implicitly,
  unmeasured, and with the core pinned at 100% believing it is behind.
- `AMIGA_LOCK_BUDGET_CCK` is 4096 colour clocks, chosen as "about a millisecond
  of chipset time" at the Pi's 250 ns/CCK. At 1449 ns/CCK that is **5.9 ms**,
  and every CPU access to the classic domain queues behind it.

The boot behaves accordingly: `amigavideo.hidd` takes **3708 ms** in
`InitResident`, `vcgfx.hidd` begins at 12.9 s, and the m68k then sits at
`pc=04248c9c` for the remaining 103 s of the run.

## `bellatrix.chipdiv=N`

One boot argument, read in `src/machine/options.c`, applied in
`amiga_clock_advance_wall()` by multiplying the counter frequency rather than
dividing the result -- so the remainder is still carried at full precision and
1/N of real time is exact to the counter tick. Nothing inside
`amiga_clock_step()` changes: a colour clock still costs what a colour clock
costs, and Rigel's cycle-exactness is untouched. What changes is how many of
them a second of real time buys.

Default 1, so hardware is unaffected. `BELLATRIX_CHIPDIV` sets it for
`./run.sh`; on a card it goes through `BELLATRIX_CMDLINE_EXTRA`.

Same host, same image, `bellatrix.chipdiv=8`:

```text
[BELLATRIX:RIGEL:PERF] 8000388 CCK in 15814 ms over 780102 calls
                    -> 1976 ns/CCK, 505893 CCK/s (14% of realtime), 10 CCK/call
```

at 18.549 s of wall clock:

- 431300 CCK/s delivered against the 443362 the scaled clock asks for --
  **97.3% of demand met, and nothing discarded.** The chipset now keeps its
  own time instead of permanently failing to keep someone else's.
- `amigavideo.hidd` takes **1082 ms** instead of 3708 (3.4x), the boot is past
  `vcgfx.hidd` at 6.0 s, reaches `STARTING DOS...` at 7.8 s and
  `STARTING SERVICES...` at 45.0 s -- where the divisor-1 run had not left
  `vcgfx.hidd` after 116 s.

## The cost it exposed, which is a separate change

`CCK/call` fell from 212 to 6-10 and `ns/CCK` rose from 1449 to 1976. With no
backlog the core takes the lock for a handful of colour clocks and pays a full
acquisition per handful -- the same shape as the "1 CCK/call" failure recorded
in the comment above `AMIGA_LOCK_BUDGET_CCK`, arrived at from the other
direction. Exclusive occupancy is still 85%.

For the CPU this is already the trade it wanted: the lock is held in ~20 us
slices instead of 5.9 ms ones. But the core is doing more work per colour clock
than it needs to, and the fix is not the budget -- it is a **floor**: do not
acquire at all until `pending_cck` is worth a lock. That should take occupancy
from 85% to roughly a quarter. Not done here; it is its own change and it wants
its own measurement.

`AMIGA_LOCK_BUDGET_CCK` deserves the same treatment from the other end. It is a
count of colour clocks standing in for a millisecond of real time, and that
substitution is only true at 250 ns/CCK. The perf counters already measure
ns/CCK, so the budget could be derived rather than assumed.

## Verifying that the default is inert, and one false alarm on the way

`chipdiv=1` is the default and it must change nothing. Proving that by reading
the code -- `freq *= 1` is `freq` -- is an argument, not a measurement, so:
the colour-clock totals at each `PERF` boundary are **identical** across every
divisor-1 run on both binaries, 4000421 / 8000615 / 12000809, at 212-215
CCK/call and 18-20% of realtime. The clock produces the same sequence it did
before the change.

The boot did not look identical, and that nearly got read as a regression. Two
divisor-1 runs on the new binary stalled inside `amigavideo.hidd` where the
first baseline run had left it after 3702 ms. What made the difference legible
was the `[BOOT] Build ID` line: the baseline had run on the image that happened
to be sitting in `out/images/`, `a1cdab38`, and the new runs on `a305ceb6`.
Rebuilding from a stashed-clean HEAD reproduced `a1cdab38` exactly -- so the
pre-existing image *was* HEAD -- and that binary then stalled inside
`amigavideo.hidd` too.

**Same build ID, two outcomes.** The stall is the machine's own intermittency
at divisor 1, and it pre-dates this change. It is also a further reading of the
same measurement: at divisor 1 a boot mostly does not get anywhere, and the
run that reached `display takeover` at 242 s did so at `chipdiv=8`.

The trap generalises past this issue: **an image left in `out/images/` is not
evidence of the tree it is being compared against.** The build ID is printed on
the thirty-something line of every boot and settles it in one grep.


# 2026-08-31: the floor, and what it is actually worth

The cost the divisor exposed has a shape, and the arithmetic names it. Two
`PERF` lines from the same machine, one saturated and one keeping up:

```text
212 CCK/call at 1449 ns/CCK  ->  307188 ns per call
 17 CCK/call at 2046 ns/CCK  ->   34782 ns per call
```

Solve for a fixed cost per call and a marginal cost per colour clock: **11 us
fixed, 1397 ns marginal**. At 17 colour clocks a call, two thirds of every call
into Rigel is entry and exit. The core that keeps up is not doing more work --
it is paying the same overhead twelve times more often.

The cause is the clamp in the drain, `if (pending_cck < quantum) quantum =
pending_cck`, which exists so a step never runs past the time that has actually
elapsed. A core that keeps up finds a handful of colour clocks banked each pass
and spends a whole step on them.

## The floor is not a number anyone chooses

`amiga_clock_quantum()` is already the distance to the next thing Rigel
considers observable. **A step shorter than that cannot produce an event** --
no VBLANK, no copper wake, no blitter completion, no interrupt -- so declining
to take it costs nothing that can be seen. `bellatrix.chipfloor` holds the core
back until `pending_cck` reaches the quantum.

One thing it does cost, and it is why this is opt-in rather than the default:
the beam registers move continuously and are not a deadline. A CPU polling
VPOSR with nothing else programmed reads a position up to `AMIGA_MAX_STEP_CCK`
(512, about two PAL scanlines) behind real time. Whenever anything *is*
programmed the quantum collapses toward 1 and the lag with it. That trade is
the user's to make, per the standing rule that no Rigel performance change
regresses cycle-exactness by default.

Note also that the floor does nothing while the core is behind: at divisor 1
under QEMU `pending_cck` is pinned at the catch-up cap, far above any quantum,
so the gate never closes. It only pays when the core keeps up -- which is to
say, with a divisor, or on hardware.

## Measured: four runs, alternated, chipdiv=8, same image, host idle

| | calls | CCK/call | ns/CCK | core occupancy | display takeover |
|---|---|---|---|---|---|
| floor off | 9.43M / 8.03M | 8 / 9 | 1875 / 1893 | 79.2% / 80.0% | 137.7 / 143.8 s |
| floor on | 417K / 415K | 182 / 183 | 1563 / 1572 | 66.0% / 66.4% | 140.2 / 138.0 s |

All four stepped the same 76.00M colour clocks and all four reached
`display takeover`. So, for identical work: **20x fewer calls into Rigel, 17%
off the cost of a colour clock, and 13 points of a core given back.**

**The boot did not get faster** -- 140 s either way. That is the honest reading
and it is not a disappointment: at `chipdiv=8` the CPU is no longer starved of
the lock, so the constraint has moved to TCG and freeing chipset core time buys
nothing here. What the floor buys is headroom, and headroom only shows where
the core is the constraint.

## Where that headroom is worth something, and why it is still untested

On a Pi 3 the chipset costs 250 ns per colour clock against a 282 ns realtime
budget -- 88.7% of a core, leaving 11.3% for the CPU (above). Seventeen percent
off 250 would be 208 ns/CCK, which is 73.7% of a core and **more than doubles
what is left for the CPU**, 11.3% to 26.3%.

That number is not a prediction. The 17% was measured under QEMU, where a
fixed per-call cost is inflated by TCG far beyond what it is on an A53; on
hardware the same fixed cost is a smaller fraction of the same work and the
gain will be smaller. QEMU shows the shape, the Pi decides the magnitude.
**This wants a hardware measurement before anything is concluded from it**,
and it is the cheapest experiment on the list: one boot argument, no rebuild.
