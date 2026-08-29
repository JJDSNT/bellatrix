---
id: ISSUE-0068
title: "The integrated chipset has no clock: what it takes to test Rigel inside Bellatrix"
status: investigating
priority: high
type: research
owner: unassigned
created_at: 2026-08-29
updated_at: 2026-08-29
tags:
  - rigel
  - chipset
  - emu68
  - integration
  - measurement
blockers:
related_files:
  - src/amiga/bus.c
  - src/amiga/irq.c
  - src/machine/machine.c
  - external/emu68/src/ExecutionLoop.c
  - cmake/bellatrix-variant.cmake
  - AI_context/consolidated/rigel_initial_bellatrix_integration.md
---

# Summary

The question that opened this was practical: can NewTek's *Demo Reel 3* be used
to test the initial chipset integration? It cannot — not because the demo is
unsuitable, but because three independent pieces of the integration do not
exist yet, and the demo would fail on all three without saying anything about
what *is* integrated.

This issue records what the integration is today, what is missing, what the
legacy tree already solved for each missing piece, and what the demo is good
for once those pieces land.

# What is integrated today

Commit `18e86c0` plus the current working tree give a **spatial** integration —
address space, not time:

- decode and MMIO: `$BFD000` (CIA), `$DC0000` (RTC), `$DFF000` (custom) are
  `EXTERNAL` machine regions dispatching to `amiga_bus_read/write`, which
  forward one physical address to `rigel_mmio_read/write`
  (`src/amiga/bus.c`). Address decode is Rigel's.
- memory: `$000000-$1FFFFF` is directly mapped Chip RAM; Rigel DMA reaches it
  through the 16-bit `machine_chip_ram_read16/write16` callbacks.
- interrupts: `amiga_irq_sync()` publishes Rigel's resolved level into
  `INTF.IPL` as a pending flag, and Emu68's arbitration pulls the authoritative
  level from `amiga_irq_get_ipl()` (`external/emu68/src/ExecutionLoop.c:364`).
- logging: Rigel's `log_fn`/`log_event_fn` reach the serial line as `[rigel]`.

A Rigel boot on hardware (2026-08-29, `out/newtek-demoreel3-*.log`) shows the
apertures registered and AROS continuing to the desktop, so the composition
does not destabilise the system.

# What is missing

## 1. Nothing advances Rigel's clock

There is no call to `rigel_step()` or `rigel_step_until()` anywhere in `src/`
or in `patches/emu68/`. Rigel's registers answer, and nothing else in Rigel
moves: no beam, no copper, no blitter, no bitplane or disk DMA, no CIA timers,
no audio, and therefore no chipset-sourced interrupt will ever be published by
`amiga_irq_sync()` — the IPL path is wired to a source that cannot change.

This is the "next integration slice" named in
`AI_context/consolidated/rigel_initial_bellatrix_integration.md`, and it is
the *whole* of what stands between the current state and any observable
chipset behaviour.

The open design question there is the canonical progress unit: Emu68's
instruction count must not be mapped 1:1 to CCK.

## 2. Nothing presents a frame

Nothing reads `rigel_get_frame()` or `rigel_get_scanline()`
(`include/rigel/rigel_denise_video.h`). Even with a clock, the chipset would
render into Rigel's internal frame buffer and stop there. The framebuffer on
this port belongs to AROS through `vcgfx`, which is a different situation from
legacy, where Bellatrix owned the display outright.

## 3. There is no way to run a bootblock demo

*Demo Reel 3* is a Kickstart 1.3 bootable disk that takes over the machine.
On this port AROS owns the CPU, there is no Kickstart, and no ADF is attached
to Rigel's floppy at all — `amiga_bus_init()` configures Chip RAM and the log
callbacks and nothing else. Running it would need a floppy image bound to
Rigel, a 68000-era ROM, and something to hand the machine over — none of which
is on the roadmap for a machine whose purpose is to run AROS.

# What the legacy tree already solved

`~/bellatrix-legacy` (branch `legacy`, tag `legacy-2026-08-03`) contains a
mature version of exactly the two pieces that are missing. It is not
copy-pasteable — legacy owned the whole machine and Rigel was *the* chipset,
whereas here Rigel is an optional domain beside AROS — but the model is
directly reusable:

- **Progress unit**: `patches/0035-emu68-modeled-cycles.patch` adds
  `M68K_OpcodeCycles()` to Emu68's `M68k_Translator.c`, a per-opcode 68000
  cost table (EA table plus group-by-group costs) emitted by the JIT, and
  publishes the accumulated total through
  `bellatrix_emu68_publish_cpu_progress()` /
  `bellatrix_emu68_publish_idle_cycles()`
  (`src/cpu/emu68/bellatrix.c:142-200`). So the unresolved unit already has a
  precedent: *modelled* CPU cycles, not instruction counts.
- **Stepping discipline**: `src/machine/machine_rigel_step.c:689` —
  `machine_next_quantum()` takes the distance to
  `rigel_get_next_observable_deadline()`, and `machine_quantum_step()` advances
  Rigel by exactly that. CPU cycles decide *when* to step; Rigel always steps
  to an event boundary, so chipset timing stays cycle-exact while CPU speed
  stays approximate. `machine_flush_for_bus()` steps the accumulated remainder
  before a register access, so an MMIO read observes current chipset state —
  our `amiga_bus_read()` has no equivalent and would read stale state the
  moment a clock exists.
- **Presentation**: `machine_present_frame_from_rigel()`
  (`src/machine/machine_rigel_step.c:328`), driven from
  `RIGEL_EVENT_FRAME_READY` in the step result, with a zero-copy path when the
  host framebuffer format matches.
- Legacy also carries the parts that are *not* wanted here: multicore chipset
  ownership (`src/runtime/core_chipset.c`), RTG, input, audio and the trace
  layer (`machine_rigel_trace.c`).

# What Demo Reel 3 is good for

Run in Rigel's own harness (Musashi + Kickstart 1.3 + the two ADFs) it is a
credible chipset workload and a reasonable *oracle* for later comparison. A run
on 2026-08-29 established its requirements and behaviour:

- 512 KB Chip alone is refused: *"This Demo Requires a 1 Meg Machine"*. With
  `--slow 512` it proceeds.
- Disk 1 boots, loads the Demo Reel 3 libraries, then requests the volume
  `demoreeldata` (disk 2). With disk 2 in DF1 the run reaches the Kickstart
  insert-disk animation again — either an expected disk swap or a defect;
  unresolved.
- The workload is real: `DMACON=03f0` (disk, sprites, blitter, copper,
  bitplanes, master), ~132K register writes over 3000 frames.

That first run used the pre-built harness binary in `~/rigel` (2026-08-21),
which predates the current Rigel API, so it was rebuilt from the pin before
any of it was believed — see below.

# 2026-08-29: the harness against the pinned API

The harness had never been exercised against the current Rigel API. It is now:

```sh
git clone /home/jaime/rigel/external/musashi external/rigel/external/musashi
git -C external/rigel/external/musashi checkout 313ebf1   # the pinned commit
external/rigel/scripts/apply_musashi_patches.sh           # 7 applied
cmake -S external/rigel -B out/rigel-harness -DRIGEL_BUILD_HARNESS=ON \
      -DRIGEL_BUILD_TESTS=ON && cmake --build out/rigel-harness -j
```

- **builds clean** against the new API, no source change needed;
- **29 of 30 tests pass**. The failure is `harness_test_blitter_timing`, the
  known `blitter_estimate_cycles` undercharge (`W×H` with no slot/CCK factor
  and no channel count): +96 CCK observed against +128 on real hardware. It is
  pre-existing and unrelated to this work;
- the Demo Reel 3 run reproduces **bit-identically** with the new binary —
  same PC, same `regw=83752` at 6000 frames, same cycle count. So the API
  change did not move harness behaviour on this workload.

`out/rigel-harness/` is under the git-ignored `out/`; the only thing written
inside a submodule is the nested Musashi checkout, and
`./scripts/setup.sh --verify` still reports all series applied.

## Defect found and fixed upstream: multi-drive status and identification

Not the demo's fault, and nothing to do with Bellatrix. Kickstart 1.3, PAL
OCS, 512K Chip + 512K Slow, `--cycle-exact`, 1500 frames:

| DF0 | DF1 | result at frame 1500 |
| --- | --- | --- |
| Demo Reel 3 disk 1 | *empty* | booted; AmigaDOS asks for volume `demoreeldata` |
| Demo Reel 3 disk 1 | Demo Reel 3 disk 2 | still in the insert-disk animation |
| Demo Reel 3 disk 1 | `wb13.adf` | still in the insert-disk animation |

So it is not disk-specific: **any** second drive with media suppresses the
boot. `--log disk` names the stage:

```
DF0 only:   DSKLEN <- 4000, DSKLEN <- 4000, DSKPTH/DSKPTL <- 00002064,
            DSKLEN <- 9cbe   (DMAEN + read, length 0x1cbe)
with DF1:   DSKLEN <- 4000   ... and nothing further
```

The ROM never programmed a transfer because unselected DF1's pending
`/DSKCHG` contaminated the shared CIA status bus. Once that was selection
gated, two drive-ID defects remained: `/DSKRDY` drove ID bits with inverted
polarity, and the motor/select preamble consumed one of the 32 samples. The
fix now tracks the selected-to-deselected edge, arms the counter before the
preamble sample window, and drives `DRT_AMIGA` with the correct active-low
polarity. `test_floppy` covers the complete 32-bit sequence.

End-to-end validation used Kickstart 1.3, PAL OCS, 512K Chip + 512K Slow and
`--cycle-exact`. Workbench mounted `DemoReel3` and `DemoReelData`; a disposable
autostart copy of disk 1 then invoked `DemoReelData:Slish` directly. At frame
3500 the harness presented a 384x256 demo frame with the red countdown digit
`7` over the static effect (307,219 register writes). The original ADFs were
not modified. The upstream record is Rigel `AI_context/issues/ISSUE-0005.md`.

# Proposed sequence

1. Rebuild the harness from `external/rigel` (the pinned API) and re-establish
   the Demo Reel 3 run there. Cheap, touches nothing in Bellatrix, and it also
   answers whether the new API broke the harness.
2. On Bellatrix, land the clock slice: a modelled-cycle progress source from
   Emu68, deadline-bounded `rigel_step()`, and a bus flush before MMIO. The
   observable acceptance test is small and does not need a demo — `VPOSR`
   advancing, a CIA timer counting down, a VBLANK reaching `INTF.IPL`.
3. Presentation: `RIGEL_EVENT_FRAME_READY` to a surface AROS can show.
4. Only then does a chipset *program* make sense as a test, and the first one
   should be an AROS m68k executable that installs a copper list and writes
   `COLOR00` — a fraction of Demo Reel 3's requirements, and it fails in a way
   that names the broken part.

# Decisions taken

- Demo Reel 3 is not the first test of this integration. It is an acceptance
  target for a chipset that already has a clock, a display and a boot path,
  and a harness-side oracle in the meantime.
- The progress-unit question is not open in the sense of having no candidate:
  legacy's modelled per-opcode cost is the candidate, and the argument for it
  is that Rigel is still stepped to its own event deadlines regardless of how
  approximate the CPU estimate is.

# 2026-08-29: clock slice implemented, idle policy validated

The first clock slice is now implemented in the working tree:

- Emu68 patch `0017-publish-modeled-cpu-progress-to-the-machine.patch`
  accumulates the legacy approximate 68000 opcode costs and periodically
  publishes the absolute count from `MainLoop`. The callback preserves the
  pinned m68k context, `x12`, and the JIT zero vector across the ordinary ABI.
- `src/amiga/bus.c` converts two modelled CPU cycles to one CCK, advances Rigel
  in bounded quanta no larger than the next observable deadline, synchronises
  IPL after each step, and flushes pending time before every Rigel MMIO.
- Clock activation is lazy. Before the first CIA, RTC, or custom-register MMIO,
  CPU time is measured but `rigel_step()` is never called. The first MMIO
  establishes the chipset time-domain epoch and discards the unobservable boot
  backlog; subsequent CPU progress keeps the chipset running asynchronously.
  This avoids both continuous chipset cost for an AROS workload that never uses
  it and an arbitrarily large catch-up pause on a first late access.

Validation:

```text
./scripts/setup.sh --verify
    emu68: already applied (17 patches)
CONFIG_RIGEL=1 ./scripts/build.sh
    Built target Emu68.elf
    out/images/Bellatrix.img
timeout 60 ./run.sh --headless
    [BootUI] [00:06.682] retargeted to RGB32 framebuffer
    [BootUI] STARTING SERVICES...
```

That boot emitted neither `clock armed by first MMIO` nor `clock active`, while
AROS reached services. This is direct evidence that no `rigel_step()` occurred
in the no-chipset-call path. An earlier eager-stepping build remained inside
Rigel's Denise framebuffer/beam work and did not reach the AROS bootstrap in 60
seconds, which is why idle activation is a correctness/performance requirement
for this integration rather than a later optimisation.

The compile-time modelled-cycle instrumentation still adds a small fixed JIT
cost even before activation. The expensive chipset simulation is absent; a
block-level cycle accumulator can remove most of the remaining accounting cost
later without changing the lazy-clock contract.

## Opt-in end-to-end acceptance

`CONFIG_RIGEL_SELFTEST=1` adds a destructive early-boot diagnostic that is
compiled out by default. It performs CPU-visible MMIO through Bellatrix's bus,
starts CIA-A Timer A, enables master plus vertical-blank interrupts, publishes
2000 modelled CPU cycles (1000 CCK), and then checks the live beam, timer,
Rigel interrupt request, and the IPL exposed by Bellatrix's IRQ boundary.

```text
CONFIG_RIGEL=1 CONFIG_RIGEL_SELFTEST=1 ./scripts/build.sh
timeout 12 ./run.sh --headless

[BELLATRIX:RIGEL:SELFTEST] PASS time=1000 beam=0000->042e \
    cia_ta=37 intreq=0020 ipl=3
[BELLATRIX:RIGEL] first VBLANK at 59474 CCK
```

This closes the clock slice's original observable acceptance criteria. VHPOSR
is used for the short beam test because VPOSR exposes only the high vertical
position bit and therefore legitimately remains unchanged across the first
four scanlines.

# 2026-08-29: idle time reaches the chipset, and Rigel is the default build

The clock slice had a hole in it that the selftest could not see, because the
selftest never stops the CPU. Chipset time is derived from modelled CPU cycles
and a stopped CPU retires none, so a guest that executed `STOP` and waited for
a chipset interrupt would wait forever: the chipset that had to raise it was
not running, and nothing was left that could wake the sleep. `docs/
legacy-emu68-patches.md` records this as a hard deadlock, and the legacy branch
had already met it -- its `0020-emu68-stop-liveness.patch` is where the shape of
the fix comes from.

Patch `0019-let-chipset-time-pass-while-the-cpu-is-stopped.patch`:

- While a chipset is running, `EMIT_STOP` no longer sleeps. It ends the
  translation unit with PC back on the `STOP`, credits 512 modelled cycles
  (256 CCK) as the idle slice, and lets `MainLoop` advance Rigel and
  re-dispatch the unit. Crediting the true zero cycles a stopped CPU retires
  freezes the machine exactly as thoroughly as sleeping.
- With no chipset running the core still parks on `WFE`, woken by the SEV a
  platform interrupt carries. Nothing needs time to pass, and spinning would
  cost a whole core to an idle desktop. `CHIPSET_ACTIVE` in `M68KState` is what
  the emitted code reads to tell the two apart; `amiga_clock_observe()` sets it
  when the first MMIO arms the clock.
- Leaving PC on the `STOP` has a consequence that is not optional to handle:
  `MainLoop` can accept the interrupt before re-dispatching the unit, and would
  then stack the address of the `STOP` itself, so `RTE` would return to it
  forever. `STOPPED` records the retirement still owed and `ExecutionLoop` pays
  it before building the frame -- on the taken path only, since a masked
  interrupt does not end a stop.
- The progress gate moved from retired instructions to modelled cycles, for the
  same reason: a stopped CPU retires no instructions, so the old gate would
  never have fired during the case it exists for.

The route not taken was calling a C helper from the wait loop. It preserves
upstream's `STOP` semantics exactly and needs no `STOPPED` flag, but Emu68's
translated code does not keep the C ABI: it would need the whole general
register file and the caller-saved vector registers spilled around every turn
of an idle loop, and `FP0-FP7` live permanently in `v8-v15`. Ending the unit
costs none of that.

Validation:

```text
CONFIG_RIGEL=1 CONFIG_RIGEL_SELFTEST=1 ./scripts/build.sh
    [BELLATRIX:RIGEL:SELFTEST] PASS time=1000 beam=0000->042e \
        cia_ta=37 intreq=0020 ipl=3
    [BELLATRIX:RIGEL] VBLANK 1 at 59474 CCK
    [BELLATRIX:RIGEL] VBLANK 512 at 30450688 CCK
```

The idle path needed a separate experiment, because a boot with the chipset
armed never reaches an idle moment: stepping Rigel is expensive enough under
QEMU that 110 seconds of wall clock got no further than `intuition.library`.
So a diagnostic build set `CHIPSET_ACTIVE` at init without arming the stepping
-- the `STOP` path yields, the chipset costs nothing -- and the machine reached
services at its usual speed and then said:

```text
[BootUI] STARTING SERVICES...
[BELLATRIX:RIGEL] chipset advancing from a stopped CPU
```

That line is emitted from `bellatrix_emu68_publish_cpu_progress()` when
`STOPPED` is set, so it can only be reached through the yield path. It is now
a permanent one-shot report, alongside a bounded VBLANK census: on a machine
with an armed chipset that has idled, its absence is the direct evidence that
idle time is not reaching the chipset.

## Rigel is built by default

`CONFIG_RIGEL` now defaults to `ON` in `cmake/bellatrix-variant.cmake`,
`scripts/build.sh` and `scripts/setup.sh`. `./scripts/build.sh` produces
`out/images/Bellatrix.img`; `CONFIG_RIGEL=0 ./scripts/build.sh` still produces
`out/images/Emu68.img` and remains a first-class composition.

That path had been broken since the image was given its own name: with the
chipset off, `IMAGE_NAME` is `Emu68.img` and the copy into the firmware
directory had the same source and destination, which `cp` refuses and `set -e`
turned into a failed build. Nobody had hit it because nobody built the default
twice. Fixed in the same change.

# What is left

- measure and, if warranted, move modelled-cycle accounting from every opcode
  to translated-block exits
- **the armed chipset is too slow, and it is not QEMU's fault** -- measured
  below. This is what stops the integration being tested by booting a machine
  with the chipset live
- extend the Demo Reel 3 oracle with a per-frame register/DMA trace if a
  stable visual baseline is needed
- `harness_test_blitter_timing` stays red on the pin; it is the opt-in
  cycle-cost work, not a regression


# 2026-08-29: why the armed machine crawls

An armed boot got as far as `intuition.library` in 110 seconds, where a
chipset-less boot reaches services in under seven. The first guess was that
QEMU's TCG multiplies Rigel's cost. It does, but it is not the answer.

## The floor is 140 ns per colour clock, with nothing programmed

A native x86 bench (`rigel_create`, then step a second of NTSC frames with the
chipset in reset, no DMA, no display, nothing on screen):

```text
deadline-bounded    3568440 CCK ->  7.13 M CCK/s  (201% of realtime, 140 ns/CCK)
quantum 512         3568640 CCK ->  7.16 M CCK/s  (202% of realtime, 140 ns/CCK)
quantum 1           3568440 CCK ->  3.02 M CCK/s  ( 85% of realtime, 331 ns/CCK)
one big step        3568440 CCK ->  7.20 M CCK/s  (203% of realtime, 139 ns/CCK)
```

Realtime is 282 ns/CCK. So an idle Rigel is only 2x faster than realtime on a
modern x86 desktop. Put the same code on the target and there is no headroom
left:

| host | ns/CCK | share of realtime |
|---|---|---|
| this x86 desktop, native | 140 | 200% |
| Raspberry Pi 3, measured earlier | ~1080 | ~26% |
| QEMU on this desktop, measured from the armed boot (91352064 CCK in ~120 s) | ~1310 | ~21% |

**QEMU is only about as slow as the real Pi here**, so the crawl reproduces on
hardware. It is a Rigel cost, not an emulation artefact.

## Every colour clock polls every domain

`gprof`, same idle workload, per-CCK call counts confirmed against the total:

```text
 14.5%  agnus_slot_scheduler_step                1 per CCK
 13.2%  rigel_denise_framebuffer_sync_from_beam  1 per CCK
 10.5%  beam_step                                1 per CCK
  7.9%  blitter_is_busy                       2.25 per CCK
  6.6%  refresh_dma_owns_slot                    1 per CCK
  5.3%  rigel_copper_domain_step                 1 per CCK
  5.3%  rigel_denise_compositor_tick             1 per CCK
  2.6%  rigel_dma_domain_read_dmacon             1 per CCK
```

There is no event skipping. `rigel_get_next_observable_deadline()` exists at the
API and Bellatrix already steps to it, but inside the step the loop still walks
every colour clock and asks every domain the same question. About a quarter of
the time goes to Denise framebuffer sync, compositor ticks and repeated
`blitter_is_busy()` calls for a screen with nothing on it and a blitter that
never runs.

## The per-call cost is real too, and it is ours to trip over

`rigel_step()` reads eight pieces of state before the work and compares them
after, including two IPL priority resolutions and a blitter poll. That is ~190
ns of fixed cost per call, which is why `quantum 1` costs 331 ns/CCK against
140. It does not bite while the chipset is idle -- the deadline-bounded run
matches the big-step run, so deadlines are far apart -- but Bellatrix steps
once per observable deadline, and a programmed copper or a running blitter is
exactly what brings those deadlines down to a handful of colour clocks. The
cost of the integration's stepping policy is therefore workload-dependent in a
way the idle measurement does not show.

## What this means for the integration

Nothing here is a defect in the Bellatrix side of the clock. The slice does
what it was built to do; the chipset it drives is roughly 4x too slow on the
target for the machine to run at speed. Making it faster is Rigel work --
event skipping first, and the standing rule that no Rigel optimisation may
regress cycle-exactness by default still applies, so it has to arrive as an
opt-in mode.

## Under a real workload the floor still dominates

Demo Reel 3 in the harness, KS13, 512K slow RAM, headless, 600 frames --
Kickstart booting and the demo running, with Musashi emulating the CPU on top
of the chipset:

```text
rigel-harness: 600 frames, 84988804 CPU cycles     wall 6.92 s
```

84988804 / 600 = 141648 CPU cycles per frame, which is a full-speed PAL frame,
so this is a genuine run and not a stalled one. 600 PAL frames is 42.6 M CCK:

- 86.7 fps, **1.73x realtime**, 162 ns/CCK
- against an idle floor of 140 ns/CCK, **a real workload costs only 16% more**

That is the number that decides what to do next. The chipset's cost is almost
entirely fixed per colour clock and almost independent of what is programmed,
so the idle profile above is a fair map of where a loaded machine spends its
time. Scaling by the Pi 3 figure gives ~13 fps PAL where 50 is needed: the gap
is **~3.8x**, which is a tuning target with a finish line, not a rewrite.

Caveat: the Pi figure is the ~1080 ns/CCK recorded from earlier work, not a
fresh measurement. It should be re-taken on hardware before the 3.8x is treated
as a contract.

# 2026-08-29: what Demo Reel 3 actually is, and why that changes the vehicle

The goal was restated as: run Demo Reel 3 on Bellatrix **without a Kickstart
and without an ADF** -- AROS is the Kickstart. So the demo has to arrive as
files on the boot volume and run as an ordinary program. That is worth checking
against what the disks contain, and the answer changes the plan.

- Disk 1's bootblock is the **stock AmigaDOS bootblock** (`DOS\0`, open
  `dos.library`, return its `dosinit`), not a demo bootblock. Both disks are
  plain OFS filesystems.
- The startup-sequence is `FastMemFirst / assign T: RAM: / execute ToRAM /
  LoadWB / EndCLI`. `ToRAM` assigns `DemoReel3:` and `DemoReelData:`, checks for
  1 MB, and copies the tune and library files to `RAM:`.
- The demo player is `DemoReelData:Slish`, 41560 bytes, a standard hunk
  executable (`0x000003f3`, three hunks). It opens `dos.library`,
  `graphics.library`, `intuition.library`, `icon.library`, `audio.device` and
  `timer.device`.

So the whole thing is an ordinary AmigaOS 1.3 application. **Nothing about it
needs a Kickstart image or a floppy** -- it needs an AmigaOS, which is what AROS
is here, and its files on a volume.

## But it straddles two machines

`Slish` also writes the custom chips directly, in four places:

```text
0x003994  lea $dff000,a0 ; move.w d0,$a8(a0) ; move.w d0,$b8(a0)   AUD0PER, AUD1PER
0x009440  movea.l #$dff000,a5 ; move.w #$002a,$96(a5)              DMACON
          move.w $dff01e,d0                                        INTREQR
0x0097ee  lea $dff000,a5 ; move.l ...,$80(a5)                      COP1LC
```

Paula periods, DMACON, INTREQR, and its own copper list. On Bellatrix those
writes land in Rigel, because `src/machine/` marks the custom aperture external
and `src/amiga/bus.c` forwards it. But the demo's *display* is set up through
`graphics.library`, and AROS renders that to the VC4 framebuffer. The program
would therefore run half in one machine and half in the other: a copper list
and audio DMA programmed into a chipset nobody is looking at, and bitplanes
drawn by a display that never sees them.

## Consequences for the plan

1. **Nothing reads Rigel's frame.** `rigel_get_frame()` returns RGBA pixels;
   `grep` over `src/` and `aros/` finds no caller. Whatever Rigel draws is
   invisible today. This slice is required before any visual test means
   anything, and it is required for every vehicle, not just this one.
2. **Being well-behaved is the point, and the straddle is a missing driver, not
   a property of the demo.** On an Amiga it is `graphics.library` that programs
   the chipset -- bitplane pointers, copper list, DMACON all come from it
   building a View. A well-behaved application therefore drives the chipset
   *through the OS*, which is the realistic case and a controlled one: its
   demands are modest and well defined, so "the chipset met them" is a
   checkable statement. What is missing is the AROS side of that: this target
   has no native chipset display driver, so `graphics.library` renders to VC4
   through `vcgfx` and Rigel's Denise is never given anything to draw.

The corrected conclusion: **Demo Reel 3 is a good vehicle**, and making it work
means supplying the producer as well as the consumer.

# 2026-08-29: how to give Rigel a screen

Both ends already exist. Only the bridge does not.

## The producer exists upstream: `amigavideo`

`external/aros/arch/m68k-amiga/hidd/amigavideo/` is AROS's native Amiga display
HIDD -- ~6000 lines, with its own bitmap class, blitter and compositor, working
on real Amigas. It programs bitplanes, the copper and DMACON exactly as the
hardware expects, which on this machine means it programs Rigel: the custom
aperture is already marked external and `src/amiga/bus.c` already forwards it.

Build it for `m68k-emu68` and `graphics.library` output goes through the
chipset. That is what turns a well-behaved application into a chipset test.
`arch/m68k-amiga/` also carries `cia/`, `graphics/` and the rest of the classic
arch, and `workbench/devs/AHI/Drivers/Paula/` is the matching audio driver.

## The consumer exists in our own tree: the HVS overlay

`aros/arch/m68k-emu68/hidd/vcgfx/vcgfx_hvs.c` already programs the Pi's
Hardware Video Scaler directly, and already has what is needed:

```c
/* Zero-copy overlay plane (windowed GL): composited above the fb
 * plane, below the cursor. Dest != src size = HVS-scaled (upscale only). */
BOOL vc4_hvs_overlay(struct VideoCoreGfx_staticdata *xsd,
                     const struct vc4gfx_overlay *ovl);
```

It takes a physical address, a pitch, a source size and an on-screen size;
updates to a live overlay are patched in place and latch at vblank. A 320x256
Denise frame scaled to the desktop by the same hardware that already composites
everything else, with no copy and no fight over the AROS framebuffer, is
exactly what this is.

## The bridge is the only new thing

Rigel's frame buffer lives in ARM memory, inside the Rigel context on Emu68's
heap. The HVS needs its physical address. AROS owns the HVS display list. So
Bellatrix has to publish a descriptor -- physical address, pitch, width,
height, active -- that the AROS side reads and hands to `vc4_hvs_overlay()`.
The mechanism for that already exists too: patch `emu68/0012` lets the machine
serve trapped guest accesses, so a small Bellatrix-owned register block is the
natural carrier.

## Four things that will bite

- **Coherency.** The HVS reads the plane by DMA, so Rigel's frame buffer has to
  be non-cacheable or explicitly flushed. This is the problem already open in
  `AI_context/consolidated/vc4_memory_coherency_upstream.md` and patch
  `emu68/0018-map-vc4-memory-normal-non-cacheable.patch`.
- **QEMU has no HVS.** `vcgfx_hvs.c:1180` reports `no HVS found (ID=...) - QEMU
  or unmapped, skipping`. The overlay half can only be tested on hardware.
- **Two display drivers.** `amigavideo` and `vcgfx` would both be display
  drivers in one AROS. On a real Amiga with a graphics card that is the normal
  arrangement and AROS's monitor handling covers it, but it decides which
  screen is on the display and needs to be got right rather than assumed.
- **Speed.** A well-behaved application driving the chipset at PAL rate needs
  the chipset at realtime, and it is ~3.8x short on a Pi 3 (ISSUE-0006 in
  Rigel).

## Overlay or source switch: both, in that order

The `legacy` branch had the other answer, and it is worth knowing before
choosing. `machine_present_frame_from_rigel()` there is one presenter with two
sources and no compositing:

```c
use_rtg = bellatrix_rtg_get_frame(&rtg_frame) != 0;
if (use_rtg) { PAL_Video_PresentRGBARegions(rtg_frame.pixels, ...); return; }
/* falls through to presenting Denise via rigel_get_frame() */
```

The switch is `RTG_REG_ENABLE` (offset 0x10) on a **virtual** Zorro III board
that Bellatrix implemented, written by the guest. RTG wins when enabled, Denise
is the default. It does not transplant: there is no virtual board here, `vcgfx`
talks to the real VC4, and AROS owns the display outright. What transplants is
the policy -- one source at a time, chosen by the guest -- and here that policy
belongs to AROS's monitor handling between `amigavideo` and `vcgfx`, not to us.

**The two are stages, not alternatives.** The switch is how the finished
machine behaves. The overlay is how it gets built, and it should come first:

- **It decouples the two halves.** The overlay shows whatever Denise is
  producing, whoever programmed it. A picture can reach the screen before
  `amigavideo` exists and before the monitor question is settled -- a test
  program poking `$dff000` from the Shell would appear immediately.
- **It keeps the desktop alive.** Under a full takeover a wrong bitplane
  pointer is a black screen with no way left to see anything. With an overlay
  the Shell is still there and the serial log still flows. For bring-up that is
  worth more than correctness of the final arrangement.
- **The AROS side is small, not a driver.** The plane is driven by an ordinary
  HIDD attribute on the on-screen bitmap, already used by `vc4gallium`:

  ```c
  struct vc4gfx_overlay {
      ULONG ovl_Phys;                 /* ARM phys of the pixel data */
      ULONG ovl_Pitch;
      ULONG ovl_Width, ovl_Height;    /* source pixels */
      LONG  ovl_X, ovl_Y;             /* position in fb coordinates */
      ULONG ovl_DestW, ovl_DestH;     /* larger than source = HVS upscale */
  };
  /* OOP_SetAttrs(bitmap, aHidd_VideoCoreGfxBitMap_Overlay -> &desc) */
  ```

Two costs to take deliberately:

- **Do not start zero-copy.** `ovl_Phys` is scanned by the HVS through DMA, and
  Rigel's frame buffer comes from `amiga_bus_alloc()` -- Emu68's cached TLSF
  heap -- with no way to tell which allocation is the framebuffer. Copy each
  finished frame into one dedicated non-cacheable buffer instead: 320x256x4 is
  320 KB, 16 MB/s at 50 Hz, and it removes the coherency question entirely.
  Zero-copy is an optimisation for later, and it needs Rigel to be told where
  to put its framebuffer.
- **QEMU has no HVS.** It does have a framebuffer -- AROS shows a picture under
  it -- so this constrains the *overlay*, not the display work as a whole. See
  the correction in ISSUE-0073: the ARM side can blit into Emu68's own
  framebuffer before `vcgfx` retargets, which makes the first visual test
  QEMU-testable with `screendump`.

## The display work has its own pair of issues

The plan below stays as the shape of the bring-up, but the detail has moved into
a pair with the same split as the audio one, for the same reason:

- **ISSUE-0072** -- what the HVS can actually scan out and what feeding a plane
  costs. Host side, no Rigel, workable now.
- **ISSUE-0073** -- how Denise connects to it. Carries the boundaries and the
  finding that matters: the audio design's parameter hand-off does **not**
  transfer cleanly, because the HVS has no indexed pixel format and so cannot do
  planar bitplanes, palette lookup, HAM or dual-playfield priority. Planar to
  chunky is software either way. What does transfer is that the renderer is a
  policy, not an architecture, with an automatic fallback on Rigel's own state.

## Suggested order

1. **Prove Denise produces pixels, with no display at all.** On
   `RIGEL_EVENT_FRAME_READY`, call `rigel_get_frame()` and report the descriptor
   and a cheap non-black pixel census to the serial line. The analogue of the
   clock selftest: it works under QEMU, it is small, and it tells us whether the
   input side is real before any display work starts.
2. **A picture, under QEMU.** Not the HVS -- see the correction in ISSUE-0073.
   The ARM side can blit into Emu68's own framebuffer before `vcgfx` retargets,
   and after that an AROS display driver modelled on our `fbgfx` presents the
   frame from guest-visible RAM. The HVS overlay is then the same driver
   presenting through a plane instead of a copy, on hardware.
3. **Build `amigavideo` for this target**, so `graphics.library` -- and
   therefore a well-behaved application -- draws through the chipset rather
   than through `vcgfx`.
4. **The source switch**, once there are two real display drivers: AROS's
   monitor handling decides which one is on the panel, and the overlay becomes
   the debugging view rather than the display.
5. **Sound**, once the picture is confirmed. It is not the same shape as the
   picture and it has its own two issues: **ISSUE-0070** makes the Bellatrix
   AHI backend correct with no Rigel involved, and **ISSUE-0071** decides how
   Paula connects to it -- preserving AUD0..AUD3 as four independent sources
   until the AHI mixer, rather than the pre-mixed stereo pair this issue first
   assumed. The split is deliberate: 0070 can be worked now, and keeping them
   apart stops decisions taken for Paula from degrading the HDMI/PWM drivers.

   **Mind the direction.** Paula is a *source* and AHI is the output stack: the
   guest programs Paula, Rigel synthesises, and the bridge plays the result
   into AHI as an ordinary client. AROS's `workbench/devs/AHI/Drivers/Paula/`
   is the opposite -- an AHIsub that sends AHI's output *to* Paula hardware --
   and has no place here at all. The producer on the guest side is the classic
   `audio.device` in `arch/m68k-amiga/devs/audio/`, which writes the AUDx
   registers and therefore reaches Rigel; it is the audio twin of `amigavideo`
   and comes from the same classic-arch package.


# 2026-08-29: the plan, in priority order

The goal this serves: **Demo Reel 3 running on Bellatrix through the chipset,
with no Kickstart and no ADF.** Everything below is ordered by what unblocks
what, not by what is interesting.

The direct ARM-side blit into Emu68's framebuffer is **dropped**. It would show
a picture within hours but nothing built for it survives into the next step,
and the steps below are all reusable.

## Phase 1 -- a known producer, and a census

A small program run from the AROS Shell that writes `$dff000` directly: bitplane
DMA on in DMACON, one bitplane pointer into chip RAM, a filled pattern,
COLOR00/COLOR01. Plus a report on `RIGEL_EVENT_FRAME_READY` giving the frame
counter, the dimensions from `rigel_denise_get_video_desc()`, a non-black pixel
count and a cheap checksum.

**Why first.** It is the first time anything drives Denise at all, it exercises
MMIO to clock to Denise end to end, and above all it is the only way to tell a
working presenter from a broken one in phase 2. Without a producer whose output
is *known*, a blank screen has three possible causes and no way to choose.

**Done when** the census reports a stable non-black count matching the pattern,
under QEMU.

**Size:** small. No new driver, no display.

## Phase 2 -- the bridge and the presenter

Bellatrix copies each finished frame into guest-visible RAM and publishes a
descriptor -- physical address, pitch, width, height, active -- through the
machine's trapped-access register block (patch `emu68/0012`). An AROS display
driver modelled on `aros/arch/m68k-emu68/hidd/fbgfx/` presents it.

**Why here.** The descriptor is the same one the HVS overlay needs later, so it
gets designed once; and the driver is the presenter the final design needs
anyway (see the display plan in ISSUE-0073).

**Done when** phase 1's pattern is on screen under QEMU, captured with
`screendump`, with the desktop still usable.

**Size:** medium, and well-bounded -- `fbgfx` is the worked template.

## Phase 3 -- amigavideo

`external/aros/arch/m68k-amiga/hidd/amigavideo/`, so that `graphics.library`
draws through the chipset instead of through `vcgfx`. **Spike first**: try
building it for `m68k-emu68` and find out what it assumes about a real Amiga
before committing to the phase. Time-box the spike.

**Why here and not earlier.** It is the only step with unbounded depth, and
phases 1 and 2 make its failures legible: with a presenter already working, a
broken `amigavideo` shows a wrong picture instead of no picture.

**Done when** an AROS screen opens on the Amiga monitor and appears through the
phase-2 presenter.

**Size:** large, and the least predictable thing here. ~6000 lines of upstream
code that has never been built for this target.

## Phase 4 -- Demo Reel 3

Both ADFs extracted to the boot volume, the `DemoReel3:` and `DemoReelData:`
assigns, `Slish` run from the Shell. No Kickstart and no ADF at run time, as
asked.

**Done when** it runs and its visuals come through Denise.

# Not on the critical path

These are real work, and sequencing them into the line above would be a mistake:

- **Rigel performance** (Rigel's ISSUE-0006). It decides whether Demo Reel 3 is
  *watchable* -- ~13 fps against 50 on a Pi 3 -- not whether any of the above
  works. It is upstream work in another repository and can run in parallel from
  the first day.
- **The audio pair** (ISSUE-0070, ISSUE-0071). ISSUE-0070 needs no Rigel at all
  and can start whenever someone wants; ISSUE-0071's boundary D means audio is
  not gated on performance either.
- **The HVS overlay** (ISSUE-0072). A hardware-only optimisation of phase 2's
  presenter, not a separate design.


# 2026-08-29: the amigavideo spike

Phase 3's question was "does ~6000 lines of upstream m68k-amiga code build for
this target, and what does it assume about a real Amiga". Answered.

## It compiles unchanged

```text
make -C out/build/aros kernel-amiga-m68k-amigavideo
    Building Module  AROS/Devs/Drivers/amigavideo.hidd ...
    exit=0
```

0 errors, 26 warnings, all of them upstream's own (`OOP_GetAttr` called with a
`struct BitMap **`, `CloseLibrary` with an `IntuitionBase *`). Output is a
61.4 KB `amigavideo.hidd` in our `emu68-m68k` distribution tree. **No patching
of upstream was needed and the build takes minutes.**

Its includes are all standard AROS plus `hardware/custom.h`, `hardware/cia.h`,
`hardware/intbits.h` and `proto/cia.h` -- nothing that assumes a machine we do
not have.

## The cost is not the driver

Three things the spike turned up, and none of them is in `amigavideo/` itself:

**1. It is a resident HIDD upstream, not a disk driver.**
`arch/m68k-amiga/boot/mmakefile.src` lists it in `KHIDDS`, linked into the ROM.
Our target loads `vcgfx` from `Devs/Drivers` with a `Devs/Monitors/VideoCore`
descriptor beside it, and builds `fbgfx` resident in the kernel ELF. So this
needs a monitor descriptor and a decision on which of the two shapes to use.

**2. `graphics.library` on m68k-amiga is chipset-flavoured.**
`arch/m68k-amiga/graphics/` overrides `vbeampos`, `waitblit`, `setchiprev` and
`bltclear`, all of which have generic counterparts in `rom/graphics/` that our
target uses today. `coppersupport.c` is Amiga-only but is internal to that
directory -- `amigavideo` does not include it, and nothing outside it does.

**3. Generic `WaitBlit()` is an empty stub.** In `rom/graphics/waitblit.c`:

```c
/*    aros_print_not_implemented ("WaitBlit"); */
/* TODO: Write graphics/WaitBlit() */
```

So a program that drives the blitter through `graphics.library` and then waits
would not wait. That is the concrete piece that has to be adopted alongside the
driver, and it is the reason item 2 is not optional.

## The one unknown left

`amigavideo_chipset.c:1768`:

```c
AddICRVector(GfxBase->cia, 2, &GfxBase->timsrv);
```

One call, needing `cia.resource` (`kernel-cia`, from `arch/m68k-amiga/cia/`)
and `GfxBase->cia` to have been set -- which is `graphics.library`'s job on
amiga-m68k. Whether our graphics sets it is unverified; if it is NULL, this is
a runtime hazard rather than a build failure, which is exactly the kind of
thing phases 1 and 2 exist to make legible.

## What this does to the plan

Phase 3 is **smaller than estimated and better understood**, but its risk moved
from build time to run time. The files to adopt are identified and few; the
failure mode left is a driver that builds, loads, and then misbehaves against a
chipset -- which is why it stays after the phases that give us a known producer
and a working presenter.

Note that the spike installed `amigavideo.hidd` into the distribution tree. It
is inert without a `Devs/Monitors` descriptor, but a `build-aros.sh full` will
now carry it.


# 2026-08-29: phase 1 done -- Denise renders

Both halves are in `src/amiga/bus.c`.

**The census** runs on `RIGEL_EVENT_FRAME_READY`: it takes `rigel_get_frame()`,
reads the top-left visible pixel as the background reference, and reports a
stride-sampled count of pixels that differ from it, plus a checksum, the
geometry and the frame flags. Six reports, then it stops, so it can be left in
a normal build. Without a display -- and on QEMU there is no HVS to build one on
-- this is the only thing that can distinguish a chipset that is rendering from
one that is merely running.

**The producer** is a display selftest under `CONFIG_RIGEL_SELFTEST`, run before
AROS is loaded so chip RAM is ours: one bitplane of vertical stripes at
`0x00010000`, COLOR00 black and COLOR01 white, `BPLCON0` for one bitplane, the
standard PAL window, and DMACON with DMAEN | BPLEN | COPEN.

## Result

```text
[BELLATRIX:RIGEL:DISPLAY] programming one bitplane
[BELLATRIX:RIGEL:CENSUS] frame=1 352x256 pitch=4096 bg=00000000 non-bg=704/1887 sum=eca14000 flags=08
[BELLATRIX:RIGEL:CENSUS] frame=2 352x256 pitch=4096 bg=00000000 non-bg=704/1887 sum=eca14000 flags=08
[BELLATRIX:RIGEL:CENSUS] frame=3 ... identical
[BELLATRIX:RIGEL:CENSUS] frame=6 ... identical
```

Six consecutive frames, byte-identical by checksum, 704 of 1887 sampled pixels
carrying the foreground colour, `flags=08` = `RIGEL_FRAME_COPPER_ACTIVE`. The
chipset turns register writes into a stable image, and the copper is running.

## What the census caught on the first attempt

The first version had no copper list, and reported:

```text
frame=1  non-bg=704/1887  sum=eca14000
frame=2  non-bg=132/1887  sum=0989bc00
frame=3  non-bg=0/1887    sum=00000000   (and every frame after)
```

Correct, then partial, then blank. **`BPL1PT` is not reloaded between frames**:
Agnus advances it as it fetches and leaves it past the end of the data, and on
real hardware it is the copper that rewrites it every vertical blank -- which is
why every Amiga display has a copper list. So the defect was in the producer,
and Rigel was right.

That is exactly what phase 1 exists for. A blank screen in phase 2 would have
had three possible causes; now it has one fewer, and the failure was legible
because the producer's output was known in advance.


# 2026-08-29: phase 2a done -- the frame reaches the guest

`src/amiga/frame.{c,h}`. Two regions, installed above the classic 24-bit
domain, because `$01000000` is exactly where the machine stops being an Amiga
and starts being ours:

```text
[BELLATRIX] machine map, 10 regions:
...
[BELLATRIX]   $01000000-$011fffff DIRECT   Denise frame aperture (host $00de7000)
[BELLATRIX]   $01200000-$01200fff EXTERNAL Denise frame descriptor
[BELLATRIX:RIGEL:FRAME] publishing 352x256 pitch=4096 at $01000000, descriptor at $01200000
```

The aperture is a 2 MiB buffer Bellatrix allocates and hands to
`machine_region_install()` as a DIRECT region, so the m68k reads it as ordinary
memory with no fault per access. The descriptor is an EXTERNAL page serving
magic, version, base, pitch, width, height, flags and a frame counter, at any
access width, so a consumer needs to agree on nothing but the byte offset.

## Why a copy, and why that is not a compromise

Rigel renders into a buffer of its own, in Emu68's heap, valid only until the
next `rigel_step` and deliberately outside the guest's address range (patch
`emu68/0007`). A consumer needs the opposite of all three: a stable address, a
stable lifetime, reachability from the m68k. One copy per finished frame buys
all of them, and it removes the coherency question for whoever reads it. There
is also nothing to be zero-copy about yet: Rigel offers no way to say where it
should render, which is part of what its ISSUE-0008 is about.

The publisher copies what fits and reports what it copied rather than refusing
a frame whose geometry grew -- a consumer reading `height` from the descriptor
draws a short image, where one that assumed a size would draw someone else's
memory.

## Still to do in phase 2

The presenter: an AROS display driver modelled on
`aros/arch/m68k-emu68/hidd/fbgfx/`, reading the descriptor at `$01200000` and
the frame at `$01000000`. Everything below it is now in place and verified.
