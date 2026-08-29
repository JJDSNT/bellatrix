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

# What is left

- measure and, if warranted, move modelled-cycle accounting from every opcode
  to translated-block exits
- define STOP/idle publication: a stopped CPU must still allow an already
  activated chipset event to wake it
- extend the Demo Reel 3 oracle with a per-frame register/DMA trace if a
  stable visual baseline is needed
- `harness_test_blitter_timing` stays red on the pin; it is the opt-in
  cycle-cost work, not a regression
