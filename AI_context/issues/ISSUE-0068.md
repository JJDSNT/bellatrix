---
id: ISSUE-0068
title: "The integrated chipset has no clock: what it takes to test Rigel inside Bellatrix"
status: investigating
priority: high
type: research
owner: unassigned
created_at: 2026-08-29
updated_at: 2026-08-31
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

> **Both patches named in this section were deleted on 2026-08-30 (`50ede60`).**
> Chipset time is wall-clock on a core of its own, so nothing wants a modelled
> cycle count: `emu68/0017` cost three ARM instructions per translated m68k
> instruction to keep one, and `emu68/0019` kept a stopped CPU spinning so it
> would keep moving. `EMIT_STOP` parks in WFE again and the chipset core's IPL
> assert sends the event. What follows is why they existed, not what is built.


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

## Selecting the chipset at boot

*2026-08-31.* `CONFIG_RIGEL` had been doing two jobs: whether the image carries
Rigel, and whether the machine it boots has one. Those are different questions
and only the first belongs to the build -- a card in someone else's hands
cannot be rebuilt to answer the second, and the two compositions are exactly
what an investigation wants to alternate between.

So the second job moved to the kernel command line, as the bare word `rigel`.
Absence is the whole of the "off" case: there is no `rigel=0` to get wrong and
no second spelling that has to agree with the first. It replaces
`bellatrix.rigel=1`, which only ever told AROS what the image had already
decided.

`src/machine/options.c` is the one reader on the host side. It is called from
Emu68's `parse_cmdline` (patch 0025), which runs before the MMU is programmed,
before the secondary cores are released and long before the first instruction
is translated -- so everything downstream is an ordinary runtime test of a
value that never changes again:

- `machine.c` installs `classic_map[]` or `plain_map[]`, and calls
  `amiga_frame_init()`/`amiga_bus_init()` only for the first. The vector page
  is now described once, outside both maps, because it is the one page they
  agree on -- and the fault-driven diagnostic works in either composition as a
  result.
- `bellatrix_chipset_core_entry()` and `amiga_console_run_on_core()` hand cores
  2 and 3 straight back, so a chipset-less boot parks them in Emu68's own WFE
  instead of one of ours.
- `EMIT_STOP` (patch 0015) picks its wait at translation time: WFI without the
  chipset, PiStorm's WFE-over-`INT64` loop with it, because a level Rigel
  publishes from another core reaches a sleeping CPU as an event and nothing
  else.
- Patch 0024's "fault at PC = 0 instead of stopping" lost its `CONFIG_RIGEL`
  half. Its argument is about booting an operating system, which both
  compositions do; leaving it coupled would have made the chipset switch
  silently change something unrelated to the chipset.

`arch/m68k-emu68/boot/boot.c` reads the same word for the guest's half of the
decision -- Fast plus a separate Chip pool, or the single heap that is both --
and both halves now print which machine they got. That matters more than it
looks: the two differ in what `AllocMem(MEMF_CHIP)` returns and in nothing else
visible at boot, so a command line that lost the word looks exactly like an
image built without the chipset.

### Closing the one direction the two halves could disagree in

The command line stays the authority, and deliberately so: a line without
`rigel` boots the chipset-less machine whatever the image carries. The gap was
only ever the other way -- the word on the line, no chipset in the image -- and
there the guest would build a chip-memory pool over addresses that fault.

**The obvious fix does not work, and it is worth writing down why.** Written up
on its own in `AI_context/consolidated/guest_device_tree_channel.md`, because it
is not about the chipset and the next person to hit it will not be reading this
issue. Publishing
the decision as a property on `/emu68`, beside `host-mem`, is unimplementable:
`dt_add_property()` edits Emu68's own parsed tree, while the guest receives
`memcpy(fdt, dt_fdt_base(), dt_total_size())` -- a byte copy of the original
blob, which has no `/emu68` node in it at all. `patches/emu68/0007` records
three earlier attempts that died on exactly this, and hit the same wall for
`host-mem`; it corrects the guest's copy of `/memory` in place instead.

That is also this fix. `bellatrix_correct_guest_cmdline()` runs in the one
window where the guest's copy of the tree exists and nothing has read it yet
(`patches/emu68/0026`), and blanks `rigel` out of the copy when the machine did
not enable it. Blanking is length-preserving, so nothing in the flattened blob
moves. The guest is then handed the command line the machine *implemented*
rather than the one that was asked for, and there is one reader of one string
again. Nothing in it can turn a chipset on.

**Also corrected while here:** `boot.c`'s `/emu68/host-mem` reader has never
once been taken, and its comment claimed it was what closed the 16 MB overlap.
It was not -- patch 0007's `/memory` trim was. The branch is kept (it costs
nothing and is right if a host ever does publish it) and now says so, because
reading it as the working mechanism is how the fourth attempt gets made. This
was found by starting to make that fourth attempt.

Validation, `t1`/`t2` from one `CONFIG_RIGEL=1` image and `t3` from a
`CONFIG_RIGEL=0` one:

```text
./run.sh --headless --no-sd
    [BELLATRIX] chipset: Rigel, asked for by "rigel" on the command line
    [BELLATRIX] machine map, 11 regions:
    [AROS/Emu68] chipset: rigel -- chip memory separate from fast

BELLATRIX_RIGEL=0 ./run.sh --headless --no-sd
    [BELLATRIX] chipset: none -- add "rigel" to the command line ...
    [BELLATRIX] machine map, 2 regions:
    [AROS/Emu68] chipset: none -- one heap, both chip and fast

CONFIG_RIGEL=0 ./scripts/build.sh
BELLATRIX_RIGEL=1 ./run.sh --headless --no-sd
    [BELLATRIX] "rigel" removed from the command line handed to the guest: \
        this image carries no chipset
    [BELLATRIX] chipset: "rigel" was asked for, but this image was built \
        with CONFIG_RIGEL=0 and carries none
    [BELLATRIX] machine map, 2 regions:
    [AROS/Emu68] chipset: none -- one heap, both chip and fast
```

All three reach `kernel.resource ready` with no region refused. Before this,
the third printed `[AROS/Emu68] chipset: rigel` against a two-region map.

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


# 2026-08-29: phase 2 done -- the guest reads what the chipset drew

`C:DeniseView` (`aros/arch/m68k-emu68/c/DeniseView.c`) reads the descriptor at
`$01200000` and the frame at `$01000000`, prints what it found, and takes the
same census Bellatrix takes on its own side. Run from the Startup-Sequence with
`BELLATRIX_BOOT_TEST=denise-view`:

```text
[BELLATRIX:RIGEL:FRAME] publishing 352x256 pitch=4096 at $01000000, descriptor at $01200000
[BELLATRIX:RIGEL:CENSUS] frame=1 352x256 pitch=4096 bg=00000000 non-bg=704/1887 sum=eca14000 flags=08
...
DeniseView: v1 352x256 pitch=4096 at $01000000, frame 2
DeniseView: bg=00000000 non-bg=704/1887 sum=ECA14000
```

**The two censuses match exactly.** Same background, same count, same checksum,
taken on opposite sides of the machine map. That is the claim phase 2 exists to
prove, and neither census could have proved it alone.

`DeniseView` without `NOWINDOW` opens a window and blits the aperture into it
with `WritePixelArray(..., RECTFMT_RGBA)`. It is a viewer, not a display
driver: when AROS draws through the chipset for real, the display driver is
`amigavideo` and what puts Denise on the panel is a plane on the video scaler.

## Two bugs the probe caught, both in the bridge

**Order.** The first run printed a valid magic and zeroes for everything else.
`amiga_bus_init()` runs the display selftest, and it ran *before*
`amiga_frame_init()` had installed the aperture -- so the frames it composed
had nowhere to be published. Invisible while the chipset kept running, because
a later frame published fine; it only appeared once the selftest started
parking the clock. `machine_init()` now installs the aperture before the
chipset, with the reason written next to it.

**Virtual against physical.** The second run had the descriptor crossing
correctly and every pixel reading zero. `MachineRegion.host_phys` is a physical
address and `tlsf` hands out virtual ones, and on this platform they are not
equal: the buffer was virtual `$00de7000` and physical `$35237000`. The fix is
`mmu_virt2phys()`.

That is exactly the confusion `region.h` warns about in the comment on the
field -- guest physical and host memory addresses "MUST NOT be treated as
interchangeable simply because an implementation can sometimes map between them
cheaply". Worth noting how it presented: **the descriptor still worked**,
because an EXTERNAL region is served by a fault handler and never consults
`host_phys`, so only the DIRECT half was wrong. A bridge that is half correct
looks like a chipset that is half working.

## Where the clock parks

The display selftest now stops the clock after composing its frames, keeping
the published frame. Stepping Rigel costs enough under QEMU that a boot which
keeps doing it never reaches a Shell -- and a Shell is where anything can read
the aperture back. The next MMIO re-arms the clock as usual.


# 2026-08-29: phase 3 -- the driver runs, and the performance wall arrives early

## What was built

`aros/arch/m68k-emu68/hidd/amigavideo/` builds upstream's sources under a
metatarget of ours plus a `DEVS:Monitors` loader, and `build-aros.sh` installs
both. The driver registers itself from its own `InitLib` with
`DDRV_KeepBootMode` and monitor ID 0, so it is added beside the VideoCore
rather than replacing it. **Rigel's output is therefore a screen, not a
window**: a screen on the AmigaVideo monitor is drawn by Denise, one on the
VideoCore monitor by `vcgfx`, and AROS's monitor handling is the display source
switch. The window in `DeniseView` is the diagnostic, not the design.

## The crash the spike predicted, exactly where it predicted it

First boot with the driver installed:

```text
[InitResident] amigavideo.hidd: MakeLibrary 0 ms, calling init @ 0x04246602
[BELLATRIX:RIGEL] clock armed by first MMIO
[rigel] event=compose ...
[AROS/Emu68] CPU exception vector 0x0000002c at PC 0xfffffffa
```

`PC 0xfffffffa` is `-6`: a library call through a null base. From
`amigavideo_chipset.c`:

```c
GfxBase->cia = OpenResource("ciab.resource");   /* NULL here */
AddICRVector(GfxBase->cia, 2, &GfxBase->timsrv);
```

Fixed by linking `cia_resource` -- and **naming it in the `#MM-` dependency
line was not enough**. That builds the object and links nothing; a resource can
arrive no other way than the resident list, because `OpenResource()` is
`FindName()` over `SysBase->ResourceList` and `lddemon` patches only
`OpenLibrary` and `OpenDevice`. It went into `CORERESIDENTS` beside
`sdio_resource` and `bwfm_resource`, which are there for the same reason.

## Two defects of ours the boot then exposed

**The Rigel event log had no bound.** One boot emitted 170274
`[rigel] event=compose` lines through `kprintf`. The serial line is the boot's
bottleneck under QEMU, so the machine looked hung and was not. Now bounded at
64 events with a line saying it went quiet.

**Arming on the first MMIO was too eager, and it costs about 4x.** AROS's
ordinary boot reads the classic domain without wanting anything of it --
`dosboot` samples CIAA.PRA and POTINP for the classic boot buttons, `battclock`
probes the RTC -- and any one of those started the chipset for the rest of the
boot. The cost is not subtle: `gfx.hidd`'s init went from milliseconds to
**10964 ms**. `amiga_access_needs_time()` now arms on what genuinely needs time
to pass: any write to the custom chips or a CIA, and a read of the beam
position. A read of a button, a port or a clock is answered from state Rigel
already holds.

## Where this stops, and why it is not a workaround away

With the eager arming fixed, the boot arms here instead:

```text
[WIFI:BWFM] resource init OK: attach deferred
[BELLATRIX:RIGEL] clock armed by a write to $00bfed01
[InitResident] battclock.resource: ...
```

`$00bfed01` is CIAA CRB, and the write is `cia.resource`'s own init -- the
resource `amigavideo` requires. So the chain is causal and has no slack in it:

```text
chipset display driver -> needs cia.resource -> whose init starts a CIA
    -> which legitimately needs chipset time -> which costs ~4x
    -> which under QEMU means the boot does not finish in a testable window
```

The classification of that write is not wrong. A running CIA timer does need
time to pass. **There is no arming rule that avoids this**, because the machine
genuinely is using the chipset from that point on -- which is the whole point of
phase 3.

## What this does to the plan

Rigel's performance work was recorded here as *not* on the critical path, on
the grounds that it decides whether Demo Reel 3 is watchable rather than
whether any of this works. **That is now wrong, and this is the correction.**

From phase 3 onward the chipset is running for the whole boot, so Rigel's
ISSUE-0006 stops being a quality question and becomes the thing that blocks
verification. Phases 1 and 2 were verifiable because the chipset could be armed
briefly and parked; phase 3 cannot park it.

Two ways forward, and they are not equivalent:

1. **Rigel's ISSUE-0006** -- event skipping in the per-colour-clock loop. It is
   the real fix, it helps every host, and the measurement that sizes it is
   already written up there.
2. **Verify phase 3 on hardware instead of QEMU.** The Pi 3 is only ~30%
   faster per colour clock than QEMU is, so this narrows the gap rather than
   closing it -- but a boot that takes four times too long is still a boot,
   where one that never finishes inside a test timeout is not.

Nothing below phase 3 is blocked: the aperture, the descriptor, the census and
`DeniseView` all work, and phase 1's producer still demonstrates the whole
render path in seconds.


# 2026-08-29: correction -- the performance numbers were measured at -O0

Everything this issue says about Rigel's speed came from a bench linked against
`out/rigel-harness`, whose `CMAKE_BUILD_TYPE` and `CMAKE_C_FLAGS` were both
empty. **It was an unoptimised build, and the numbers are wrong by about 4x.**

| | published here | correct (Release) |
|---|---|---|
| idle chipset | 140 ns/CCK, 2x realtime | **35 ns/CCK, 8x realtime** |
| Demo Reel 3, 600 frames | 6.92 s, 86.7 fps | **2.61 s, 229.9 fps, 4.6x realtime** |

Withdrawn with them: **"~3.8x short on a Pi 3"**. That rested on an earlier
~1080 ns/CCK figure for the Pi, which against 35 ns/CCK native would make an
A53 31x slower than a modern x86 -- implausible, so the Pi figure is suspect
too and has to be re-measured before any target is set from it.

## What this does and does not change

**Does not change**: everything measured on the Bellatrix side, because
`scripts/build.sh` builds Release. The ~1.31 us/CCK seen under QEMU is a real
number for optimised code under TCG, and so is the boot that reaches
`intuition.library` in 110 seconds with the chipset armed. Phase 3 is still
blocked under QEMU for exactly the reason recorded above.

**Does change**: the explanation. That slowness is now much more plausibly a
TCG artefact -- 1310 against 35 ns/CCK is a 37x emulation penalty, which is an
ordinary figure for interpreted AArch64 -- rather than evidence that Rigel is
intrinsically too slow for the target. Which reopens the question the phase 3
note closed: **hardware may simply be fast enough**, and that is now the
cheapest thing to find out.

So the two ways forward swap places. Verifying phase 3 on a Pi moves ahead of
Rigel's event-skipping work, because it is a measurement rather than a project,
and because the number that would justify the project no longer exists.

## The lesson worth keeping

A performance gate in another repository was opened with a measurement whose
build flags were never checked, and the shape of the finding -- a fixed
per-colour-clock cost that barely responds to what is programmed -- was
convincing enough that nothing about it looked wrong. The `gprof` ranking was
sound because it came from a different build; the absolutes were not, and
nothing in the recipe said which build to link against.


# 2026-08-30: instrumented before going to hardware

Every performance claim in this issue was made by timing a whole run from
outside and dividing, which averages the chipset together with a boot that is
mostly not chipset -- and which is how a measurement at `-O0` went unnoticed.
On hardware it would be worse: if the machine is too slow to reach a Shell, the
only thing that can report is the serial line during boot.

So `amiga_clock_step()` now measures its own exclusive cost with `CNTPCT_EL0`,
which is a real clock on QEMU and on hardware alike, and reports periodically:

```text
[BELLATRIX:RIGEL:PERF] 4000286 CCK in 5460 ms over 17691 calls -> 1365 ns/CCK, 732593 CCK/s (20% of realtime), 226 CCK/call
[BELLATRIX:RIGEL:PERF] 20001062 CCK in 34550 ms over 88448 calls -> 1727 ns/CCK, 578892 CCK/s (16% of realtime), 226 CCK/call
```

This is the split Rigel's own `rigel_performance_research.md` asks for before
any chipset optimisation is chosen, and it answers two questions immediately
that external timing could not:

- **226 colour clocks per call.** Many short calls would mean our stepping
  granularity is the problem; it is not, and that was the first hypothesis the
  Rigel document says to exclude.
- **The cost per colour clock grows through the boot**, 1365 to 1727 ns, as
  more chipset is programmed. A chipset whose cost were purely fixed would not
  do that, so "almost independent of what is programmed" is true only to a
  first approximation.

## What is left before a hardware run

1. **Take two images.** `CONFIG_RIGEL_SELFTEST=1` proves phases 1 and 2 cheaply
   -- it composes a known frame, parks the clock, and lets the boot finish at
   normal speed. The plain `CONFIG_RIGEL=1` image is the phase 3 case, with the
   chipset live for the whole boot.
2. **`nocomposition`** is still required on the kernel command line to see
   anything on the framebuffer (CLAUDE.md).
3. **There is no `screendump` on hardware.** The census is the visual evidence,
   and it already prints on both sides.
4. **The number to bring back is the `PERF` line.** ns/CCK on a Pi 3 is what
   the withdrawn "~3.8x short" claim needs replacing with, and it is now
   measured by the machine itself rather than derived.


# 2026-08-30: why the clock is not disarmed

The rule that arms the clock has an obvious mirror -- disarm when nothing needs
time any more -- and it was worth an hour to find out that it does not pay.

**What arms the machine is a CIA timer, and it keeps running.** The measured
boot arms on a write to `$00bfed01`, CIAA CRB, which is `cia.resource` starting
its timer. Disarming requires proving nothing needs time; a timer that is
counting does. So the disarm would not touch the case we actually measured.

**And the machine could not prove it anyway.** Rigel's public API exposes
`rigel_get_intena`, `rigel_get_intreq`, `rigel_get_ipl`, the deadlines and the
frame -- no DMACON, no CIA timer state. The condition would have to be built
from a shadow of the register writes we intercept, which is a second source of
truth for something whose failure mode is a guest that waits forever for an
interrupt that stopped being generated. That is the STOP deadlock again, in a
place with no probe to catch it.

**What the investigation did produce** is the sharpest statement of the real
problem, now written up as Rigel's ISSUE-0006: a CIA timer costs nothing per
colour clock, because `rigel_chipset_step()` accumulates the remainder and
calls `cia_step()` once per call in bulk. So this boot pays the full
per-colour-clock Agnus and Denise loop -- 1365 ns/CCK -- for a domain that is
already O(1) and a display with nothing on it.

Three other facts went to the same issue, and together they close off the
outside: our call granularity is already maximal at a measured 226 CCK per
call; `d.beam_line_end` is unconditional in `rigel_get_deadline()`, so no host
can ever be told it may skip more than a scanline; and
`agnus_slot_scheduler_step_until()` is a plain loop, so a longer quantum would
not skip work even if we were given one.

**The host cannot fix this from outside.** That is the conclusion, and it is
worth more than the code that was not written.


# 2026-08-30: the hardware answer, and it is not the one anyone was looking for

Pack B on a Raspberry Pi 3 Model B, chipset live for the whole boot:

```text
[BELLATRIX:RIGEL:PERF] 76003913 CCK in 19021 ms over 334821 calls -> 250 ns/CCK, 3995629 CCK/s (112% of realtime), 226 CCK/call
```

Flat at 250 ns/CCK across 76 million colour clocks. **The chipset runs at 112%
of realtime on hardware.**

## Rigel is not the bottleneck, and never was

| | ns/CCK | vs native x86 |
|---|---|---|
| native x86, Release, idle | 35 | 1x |
| **Raspberry Pi 3** | **250** | **7.1x** |
| QEMU on that x86 | 1365 | 39x |

7.1x for an A53 against a modern x86 is ordinary. The earlier ~1080 ns/CCK
figure for the Pi was wrong by 4.3x, and the QEMU wall was a TCG artefact, at
5.5x the Pi's cost. **Rigel's ISSUE-0006 comes off the critical path**: a
chipset with 12% headroom does not need event skipping to make this work.

## What is actually slow, and why it is not a defect

The boot takes over thirteen minutes, and the SD card reports:

```text
[SDHost00] 1024 cmds, 8020 KB, 4096 KB in 339968 ms = 12 KB/s  [wait 4540 ms, cache 47 ms, copy 2 ms]
[SDHost00]   command total 8486 ms of which transfer 4540 ms
```

4 MB in 340 seconds, of which the driver spent 8.5 seconds in commands.
**97.5% of that wall time was spent outside the driver.** The card is not slow;
the CPU is.

And it is slow by construction. Bellatrix drives chipset time from modelled
68000 cycles at two cycles per colour clock, so a chipset running at 3995629
CCK/s caps the guest at **7.99 MHz-equivalent -- 112% of a real 7.09 MHz
68000**. The same number, from the other end.

## The finding

**A cycle-exact chipset driven by modelled CPU cycles throttles the machine to
the speed of the machine it is emulating.**

With the chipset live, this is an Amiga. Exactly as fast as one, to within 12%.
That is precisely right for compatibility -- a demo written for a 7 MHz 68000
will run at the speed it was written for -- and it throws away the reason Emu68
exists, which is that the JIT runs m68k code far faster than the hardware ever
did. AROS is a system that expects the fast CPU, and on an Amiga it boots in
thirteen minutes.

Nothing here is broken. The clock slice, the STOP path, the aperture, the
census, `amigavideo` and `cia.resource` all do what they were built to do. The
architecture simply has a consequence nobody had priced, because until this
measurement the chipset's cost was believed to be the problem.

## The question this opens

It is no longer "how do we make the chipset fast enough". It is **what the
relationship between CPU time and chipset time should be**, and there are at
least three answers with different characters:

1. **Chipset on its own core.** The CPU free-runs on one core while the chipset
   keeps realtime on another. That is an accelerated Amiga -- the PiStorm
   shape -- and the `legacy` branch already built it: `src/runtime/core_chipset.c`,
   with Core 2 running the chipset. It was left behind as multicore scaffolding;
   this measurement is the argument for it.
2. **Decouple, and synchronise only at observation.** The CPU runs free and the
   chipset catches up when something looks at it. Cheap, and it changes what the
   guest observes about time, which is the thing Rigel exists to get right.
3. **Accept Amiga speed while the chipset is in use.** Correct, simple, and it
   means the desktop cannot be one of the things using it.

The choice is not obvious and it is not mine to make alone. What is now settled
is that it is a scheduling question rather than a performance one.


# 2026-08-30: the chipset received a screen

From a hardware boot, in a line that nearly went unread:

```text
[BELLATRIX:RIGEL:CENSUS] frame=1000 256x256 pitch=4096 bg=00aaaaaa non-bg=0/1369 sum=54a363aa flags=00
```

**`bg=00aaaaaa` is the Workbench grey.** It had been `00000000` in every
previous boot. Something programmed Denise's COLOR00 to Amiga grey, which
means `graphics.library` reached the chipset -- the thing phases 1 to 3 were
built to make possible, seen for the first time.

## And it explains the frozen clock

The report that came with it was that the BootUI clock had stopped while the
serial log kept flowing. That is not a hang: **`amigavideo` took the display.**
From that moment AROS draws through Denise, whose output nothing presents, so
the panel holds whatever the VideoCore last scanned out. The machine is running
and invisible.

Which is the expected consequence of finishing phase 3 without phase 2's
presenter, stated in this issue before either was built -- and still surprising
when it arrived, because it arrives looking exactly like a crash.

## The census was hiding it

The count is of pixels differing from the top-left one, so a screen filled with
a single colour reads as `non-bg=0` however loudly it says something happened.
A frame that went from black to grey is a display being set up, and the metric
threw it away.

Now the background colour is reported when it changes. **A signal that only
appears as a changed constant is still a signal**, and a census that counts
variation cannot see it by construction.

## What this makes the next step

Not "make the demo draw through Denise" -- something already does. It is
presentation: the frame is in the aperture at `$01000000`, published every
frame, and nothing puts it on the panel. On hardware that is the HVS plane
(ISSUE-0072, ISSUE-0073), and until it exists every success here looks like a
freeze.


# 2026-08-30: the demo reaches the chipset's audio

Phase 2's presenter exists (`DeniseView SHOW`, ISSUE-0073) and phase 3 has a
first real producer that is not a selftest: Demo Reel 3's player programs all
four of Paula's channels through Rigel, once `audio.device` was linked into
the ROM. The guest then dies on a library call through a null base.
**ISSUE-0079** carries it, including the structural gap it exposes -- this
port has no Paula interrupt dispatch, so `SetIntVector(INTB_AUD0 + ch)`
installs handlers nothing will ever call.

Getting there needed the machine to boot reliably, which took the whole day
and a different USB driver: **ISSUE-0078**.
