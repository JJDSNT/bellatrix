// AI_context/sprint_21.md

# Sprint 21 — Bellatrix/Emu68 overlay wiring aligned with the live fault path

## Description

This sprint focused on the remaining architectural gap between the Bellatrix
bare-metal integration and the way Emu68 itself expects variants to behave.

The immediate trigger was the comparison between:

- the Bellatrix harness, where DiagROM behaves better
- the Bellatrix bare-metal variant on top of Emu68
- the reference PiStorm integration inside `emu68/src/pistorm`

The main question was whether the current Bellatrix hooks were conceptually
correct, or whether the project was fighting the Emu68 execution model.

## Architectural conclusion

After reviewing `emu68/src/pistorm`, `emu68/src/aarch64/vectors.c`,
`emu68/src/ExecutionLoop.c`, and the Bellatrix variant code, the conclusion is:

1. **ExecutionLoop-driven time ownership is the right approach**

The Bellatrix hook in `emu68/src/ExecutionLoop.c` that computes a cycle delta
from `v30` and calls `bellatrix_bridge_cpu_progress(...)` matches Emu68's
architecture well and should remain the primary machine-time driver.

2. **The Bellatrix fault hook entry point is `vectors.c`, not `bellatrix_bus_access()`**

The live Bellatrix Emu68 path is:

- `SYSWriteValToAddr()` / `SYSReadValFromAddr()` in
  `emu68/src/aarch64/vectors.c`
- `bellatrix_bridge_cpu_access()`
- `bellatrix_machine_read()` / `bellatrix_machine_write()`

`src/cpu/bellatrix.c:bellatrix_bus_access()` was carrying important overlay
logic, but the Emu68 Bellatrix path does not actually call that function.

This meant the intended OVL/MMU synchronization logic was effectively sitting
in a non-live path.

3. **The current Bellatrix OVL behavior was narrower than both harness and base Emu68**

The Bellatrix bare-metal variant had been mapping only the first 4 KiB of page
zero to ROM when OVL was active.

That is too small.

Both the Bellatrix harness and the base Emu68 non-Bellatrix path effectively
treat overlay visibility as the full low 512 KiB boot window:

- harness: `addr < 0x80000`
- Emu68 base `SYSReadValFromAddr()`: `far < 0x80000`

So the Bellatrix bare-metal path was materially inconsistent with both.

4. **For Bellatrix, OVL must be corrected through MMU remapping, not only bridge reads**

Unlike PiStorm, Bellatrix maps `0x000000-0x1fffff` directly as chip RAM in the
MMU. That means many low-memory reads never fault through the bridge path.

So simply trying to "fix overlay in the bridge read function" is insufficient.

The right fix is to update the host MMU mapping for the full low overlay
window whenever CIA-A PRA bit 0 changes.

## What was changed

### 1. OVL sync moved onto the live Bellatrix write path

Added a new public helper:

- `bellatrix_sync_overlay_from_ciaa()`

Files:

- `src/cpu/bellatrix.h`
- `src/cpu/bellatrix.c`

This helper reads the logical CIA-A PRA state from the Bellatrix machine and
updates the MMU overlay mapping using the live machine state, instead of
depending on the dormant `bellatrix_bus_access()` path.

### 2. Emu68 Bellatrix write hook now triggers live OVL synchronization

In the active Bellatrix path in:

- `emu68/src/aarch64/vectors.c`

after `bellatrix_bridge_cpu_access(..., BELLATRIX_BUS_WRITE)`, writes to
`0x00BFE001` now call:

- `bellatrix_sync_overlay_from_ciaa()`

This is now the real OVL transition point for the Bellatrix Emu68 variant.

### 3. OVL mapping widened from 4 KiB to 512 KiB

The Bellatrix MMU overlay mapping was changed to cover:

- `0x000000-0x07ffff`

using the ROM shadow at:

- `0x00E00000`

This matches the common Bellatrix memory semantics and the base Emu68
expectation for low-memory overlay behavior.

### 4. OVL-off remap now restores low RAM and then reapplies debug traps

When OVL goes low:

- the full low 512 KiB window is remapped back to chip RAM
- the debug write traps for:
  - page zero
  - Exec JMP table page (`0x1000-0x1fff`)

are then reapplied as read-only pages

This preserves the existing debug visibility without leaving the whole low
window stuck in the wrong memory mode.

### 5. Paula chip RAM attachment gap remained fixed

The earlier Bellatrix fix was kept:

- after replacing `m->memory.chip_ram` with the real Emu68-backed RAM alias,
  Bellatrix now reattaches Paula to that backing with `paula_attach_memory(...)`

This keeps Paula DMA-side memory ownership aligned with the actual chip RAM
pointer used on target.

## Current recommendation

The recommended Bellatrix/Emu68 integration strategy is now:

1. **Keep cycle ownership in `ExecutionLoop.c`**
2. **Keep fault/MMIO ownership in `vectors.c`**
3. **Use the MMU for RAM/ROM visibility semantics**
4. **Avoid placing essential machine behavior in `bellatrix_bus_access()` unless
   that function becomes the actual live entry point**

This is much closer to the PiStorm model and is the best direction for future
fixes.

## Remaining follow-up

1. **Reset fidelity**

The Bellatrix variant still seeds `ISP` and `PC` directly from
`bellatrix_reset_isp` / `bellatrix_reset_pc` in `M68K_StartEmu()`.

That is still less faithful than the harness `pulse_reset` path and should be
revisited later if DiagROM still finds reset/OVL inconsistencies.

2. **Verify DiagROM behavior after the live OVL fix**

The next thing to validate on target is whether the previous:

- `Checking if OVL works: FAILED`

now changes, especially with `diagrom2.rom`.

3. **Consider retiring or demoting `bellatrix_bus_access()`**

Since the live Bellatrix Emu68 bridge does not currently call it, that
function should not remain the only place where critical runtime behavior is
implemented.

It can still be useful as a debug/alternate dispatcher, but it should not be
treated as the authoritative path unless the wiring changes.

## Files modified in this sprint

- `src/cpu/bellatrix.h`
- `src/cpu/bellatrix.c`
- `emu68/src/aarch64/vectors.c`

## Validation

Validation performed in this sprint:

- inspected the live Bellatrix Emu68 path
- compared it against the PiStorm integration in `emu68/src/pistorm`
- confirmed Bellatrix OVL logic had been sitting outside the live Emu68 entry
  path
- rewired OVL sync into the active Bellatrix fault hook path
- widened Bellatrix low-memory overlay remap from 4 KiB to 512 KiB

No full bare-metal rebuild or target boot validation was run in this sprint.

## Structural follow-up analysis

After the live OVL wiring work, a second pass over the Bellatrix/Emu68 stack
showed that the next likely gaps are no longer just "missing hooks". They are
mostly about model completeness in timing and DMA ownership.

### 1. Agnus DMA arbitration exists on disk but is not the live model yet

There is now a dedicated DMA arbiter implementation in:

- `src/chipset/agnus/dma.c`
- `src/chipset/agnus/dma.h`

This code already contains:

- explicit `DMACON` ownership
- filtered request gating per DMA class
- blitter-nasty priority handling
- a central slot/grant model

However, the live `AgnusState` in:

- `src/chipset/agnus/agnus.h`

still only carries a raw:

- `uint16_t dmacon`

and the live `agnus_step()` path in:

- `src/chipset/agnus/agnus.c`

does not use `AgnusDMA` as its scheduling authority.

That means Bellatrix currently has:

- a stronger DMA architecture designed
- but an older ad hoc DMA execution path still active

This is a structural mismatch and probably the biggest remaining completeness
gap.

### 2. Bitplane DMA still contains a bring-up fallback that can hide real bugs

In:

- `src/chipset/agnus/bitplanes.c`

`agnus_bitplane_dma_enabled()` still has a compatibility path that forces
bitplane DMA on when:

- `DMAEN` is set
- `BPLEN` is not seen
- but the bitplane state otherwise "looks valid"

This is logged as:

- `[BPL-DMA-FORCE]`

That fallback was useful for bring-up, but it is not a faithful DMA model.
It can hide exactly the kind of structural bug we now care about:

- missing or mistimed `DMACON` updates
- wrong Copper ordering
- wrong bus ownership

Recommendation:

- keep this fallback only as an optional debug escape hatch
- do not rely on it in the default Bellatrix target path once DMA ownership is
  centralized

### 3. Paula disk DMA is still a fake bulk-transfer model

In:

- `src/chipset/paula/paula_disk.c`

disk DMA still starts with:

- `FLOPPY_FAKE_DMA_CYCLES`

and then pre-encodes an entire MFM track into chip RAM up front.

This is pragmatic for software that only cares that data eventually appears,
but it is not close to the real Agnus/Paula DMA cadence.

Implications:

- software that polls sync/data timing can observe unrealistic behavior
- the disk side is not actually competing for DMA slots with bitplanes,
  copper, sprites, audio, or blitter
- this weakens confidence when diagnosing DiagROM or early-boot bring-up

Recommendation:

- keep the current decoder/encoder logic
- but move actual transfer pacing under a live Agnus DMA slot/grant model
- treat disk DMA as a requester, not as a bulk chip RAM memcpy with a timer

### 4. Bellatrix timing is still calibrated by a simple instruction delta multiplier

In:

- `emu68/src/ExecutionLoop.c`

the Bellatrix path advances the machine with:

- `bellatrix_bridge_cpu_progress(bela_delta * 8u)`

This is clean architecturally, but still a coarse calibration.

It assumes that the instruction counter delta from Emu68 maps well enough onto
Bellatrix machine ticks via a constant multiplier.

That may be "good enough" for broad progress, but it is a plausible source of:

- CIA drift
- serial pacing mismatch
- DMA scheduling mismatch
- Copper/bitplane edge ordering differences

Recommendation:

- keep `ExecutionLoop` as time owner
- but validate and possibly retune the multiplier against observable frame and
  beam behavior
- if needed, derive the chipset tick rate from a more direct Emu68 notion of
  elapsed core cycles rather than only JIT instruction retirement

### 5. Reset fidelity still differs from harness and base PiStorm

The Bellatrix path in:

- `emu68/src/aarch64/start.c`

still seeds:

- `ISP`
- `PC`

directly from:

- `bellatrix_reset_isp`
- `bellatrix_reset_pc`

The harness still does:

- CPU reset pulse
- vector fetch through the bus with overlay active

This means Bellatrix bare metal still starts from a precomputed reset view,
not from the actual live bus-visible memory map.

Recommendation:

- do not treat this as the first issue to solve
- but after DMA/timing cleanup, revisit reset so Bellatrix boot enters through
  the same visible memory semantics as the harness

## Recommended order of attack

The best next steps are:

1. Wire `AgnusDMA` into the live `AgnusState` and make it authoritative for
   `DMACON` and grant ordering.
2. Convert bitplane, copper, blitter, disk, sprite, and later audio activity
   into DMA requesters instead of independent enable checks.
3. Remove or gate the `BPL-DMA-FORCE` fallback once the centralized DMA path
   is working.
4. Rework Paula disk DMA so transfer visibility follows DMA grants instead of
   a fake whole-track copy.
5. Calibrate the Bellatrix `ExecutionLoop` tick multiplier against observed
   frame/beam timing.
6. Revisit reset fidelity only after the DMA/timing model is materially closer
   to the harness.

## Practical conclusion

At this point, the highest-value structural work is not another isolated hook.

It is:

- making DMA ownership genuinely centralized
- reducing timing approximations that bypass arbitration
- removing bring-up fallbacks that can mask real ordering bugs

OVL was necessary, but OVL alone is not enough to make Bellatrix/Emu68 behave
like the harness for software that is sensitive to early boot sequencing.

## Incremental DMA wiring applied

After the structural analysis above, an incremental first step was applied to
reduce the gap between the designed DMA model and the live Agnus state.

### What changed

1. `AgnusDMA` is now embedded in the live `AgnusState`

Files:

- `src/chipset/agnus/agnus.h`
- `src/chipset/agnus/agnus.c`

The live Agnus now owns an `AgnusDMA dma` member.

`dma.dmacon` is treated as the authoritative DMACON state, while the older
`AgnusState::dmacon` field is kept only as a synchronized transitional mirror
for older debug and compatibility paths.

2. Live `DMACON/DMACONR` handling now goes through the DMA module

In `agnus_write_reg()`:

- writes to `AGNUS_DMACON` now use `agnus_dma_write_dmacon(...)`

In `agnus_read_reg()`:

- reads from `AGNUS_DMACONR` now use `agnus_dma_read_dmaconr(...)`

This means the Bellatrix live Agnus path now consumes the dedicated DMA module
for register semantics, instead of open-coding DMACON behavior inside
`agnus.c`.

3. Blitter busy/zero status is now sourced through DMA callbacks

The live Agnus registers DMA callbacks for:

- blitter busy
- blitter zero

This lets `DMACONR` status bits come from the DMA module while still reflecting
the actual live blitter state.

4. Bitplane/debug readers were pointed at the new DMACON source

Files:

- `src/chipset/agnus/bitplanes.c`
- `src/debug/emu_debug.c`

Direct readers that matter for diagnostics or enable checks now use:

- `agnus_dmacon_current(...)`

instead of reading the transitional mirror directly.

5. A header-level symbol collision was resolved

`src/chipset/agnus/dma.h` had `AGNUS_DMACON_BBUSY/BZERO` names colliding with
`blitter.h`.

The DMA module now uses its own internal status names:

- `AGNUS_DMA_DMACON_BBUSY`
- `AGNUS_DMA_DMACON_BZERO`

This avoids namespace conflict without changing visible behavior.

### What this did not do yet

This step did **not** yet make the DMA arbiter drive the live raster/slot
execution.

Still missing:

- request collection from bitplanes/copper/blitter/disk/audio/sprites
- slot-by-slot grant execution through `agnus_dma_step(...)`
- removal of the `BPL-DMA-FORCE` bring-up fallback
- conversion of Paula disk DMA from fake bulk timing into granted DMA service

So this was intentionally an architectural bridge step, not the full DMA
refactor.

### Validation

Validated by rebuilding:

- `cmake --build emu68/build-bellatrix-codex`

Build completed successfully and produced:

- `emu68/build-bellatrix-codex/Emu68.elf`
- `emu68/build-bellatrix-codex/Emu68.img`

## Incremental DMA arbiter usage applied

The next cut after the live `DMACON` migration was to start using the DMA
arbiter during normal Agnus stepping, but in a deliberately narrow way.

### What changed

In:

- `src/chipset/agnus/agnus.c`

the normal per-tick Copper execution path was moved from a direct call:

- `copper_service_step(..., 1)`

to:

- `agnus_dma_step(&s->dma, 1)`

The DMA callbacks now:

- query whether the Copper should request the current slot
- service a `AGNUS_DMA_REQ_COPPER` grant by running one unit of
  `copper_service_step(...)`

The wake-up ordering remains:

- beam advance
- `copper_service_poll(...)`
- DMA arbitration/service
- bitplanes stepping

So the important "wake before video snapshot" rule is preserved, but normal
Copper execution is now slot-owned by the DMA module.

### Important scope limitation

This is **not** full DMA arbitration yet.

At this point:

- Copper is a real DMA requester/service consumer
- blitter busy/zero status is reflected into the DMA module
- but blitter is still advanced by elapsed-time stepping, not by DMA grants
- bitplanes are still fetched by the existing incremental fetch model
- disk DMA is still using the fake bulk-transfer model

This means the project now has:

- live `DMACON/DMACONR` routed through `AgnusDMA`
- live Copper slot execution routed through `AgnusDMA`

but not yet:

- a unified requester model for all DMA clients

### Validation

Revalidated with:

- `cmake --build emu68/build-bellatrix-codex`

Build completed successfully again after moving Copper execution behind the DMA
arbiter.

## Incremental bitplane DMA grant flow applied

The next structural cut moved bitplane fetch ownership behind the DMA arbiter.

### What changed

Files:

- `src/chipset/agnus/bitplanes.h`
- `src/chipset/agnus/bitplanes.c`
- `src/chipset/agnus/agnus.c`

The previous model still had:

- line setup in `bitplanes_step(...)`
- but actual word fetch progression happening directly inside
  `bitplanes_progress_fetch(...)`

That meant bitplane data visibility was still beam-timed, but not DMA-grant
timed.

This was changed so that:

1. `bitplanes_step(...)` now only latches per-line display state.
2. Bitplane fetch demand is exposed through:
   - `bitplanes_dma_request_mask(...)`
3. Bitplane fetch service is exposed through:
   - `bitplanes_dma_service_next(...)`
4. The Agnus DMA query callback now includes bitplane request bits.
5. The Agnus DMA service callback now services bitplane requests.

### Bitplane fetch granularity

The live Bellatrix model now fetches bitplanes incrementally by:

- current word index
- current plane index within that word

New state:

- `fetch_plane_index`

was added to `BitplaneState`.

So the arbiter can now grant bitplane service one plane-fetch at a time, while
still preserving the existing Denise-facing line buffer contract.

### Important limitation

This is still an incremental migration, not a full real-Agnus slot map.

Specifically:

- the arbiter now decides when a bitplane fetch may occur
- but the request/grant model is still projected onto the current Bellatrix
  line-buffer implementation
- it is not yet a cycle-accurate recreation of all Amiga DMA slot classes

Still, this is materially better than the previous structure because bitplane
visibility is no longer advanced by a direct internal loop that bypasses the
DMA arbiter entirely.

### Agnus step ordering

`agnus_step(...)` now:

- polls Copper wake state when enabled
- always advances `agnus_dma_step(..., 1)` for the current tick
- then lets bitplanes/Denise observe the updated state

This means both:

- Copper execution
- bitplane fetch service

now pass through the DMA module during the live Bellatrix path.

### Validation

Revalidated with:

- `cmake --build emu68/build-bellatrix-codex`

Build completed successfully after the bitplane DMA integration.

## Incremental blitter DMA grant flow applied

The next cut moved live blitter progress behind DMA grants as well.

### What changed

Files:

- `src/chipset/agnus/blitter.h`
- `src/chipset/agnus/blitter.c`
- `src/chipset/agnus/agnus.c`

Previously, the live path still advanced the blitter by elapsed Agnus ticks via:

- `blitter_step(&s->blitter, s, ticks)`

inside `agnus_step(...)`.

That meant:

- blitter busy duration
- `BLTPRI` influence
- contention with Copper/bitplane activity

were still structurally outside the DMA grant path.

This was changed so that:

1. the Agnus DMA query callback now includes:
   - `blitter_dma_request_mask(...)`
2. the Agnus DMA service callback now services:
   - `AGNUS_DMA_REQ_BLITTER`
3. live `agnus_step(...)` no longer advances the blitter directly by elapsed
   ticks
4. blitter progress now consumes one unit of `cycles_remaining` per DMA grant

### API shape

New public helpers were added:

- `blitter_dma_request_mask(...)`
- `blitter_dma_service_grant(...)`

`blitter_step(...)` was kept as a compatibility wrapper, but it now simply
delegates to repeated DMA-style grant service instead of implementing a
separate direct-time completion path.

### Result

At this point, the live Bellatrix Agnus path has these clients going through
the DMA module:

- DMACON / DMACONR semantics
- Copper slot execution
- bitplane fetch service
- blitter progress

This is a substantial architectural improvement over the earlier state where
only register semantics were centralized but execution still bypassed the DMA
arbiter for major subsystems.

### Remaining structural gaps

The main DMA-side gaps after this cut are now concentrated in:

- Paula disk DMA still using the fake bulk-transfer/timer model
- audio not yet expressed as real DMA requesters
- sprites not yet expressed as real DMA requesters

Outside DMA, major remaining gaps still include:

- global timing calibration of the Bellatrix `ExecutionLoop` multiplier
- reset fidelity versus the harness bus-visible reset path

### Validation

Revalidated with:

- `cmake --build emu68/build-bellatrix-codex`

Build completed successfully after the blitter DMA integration.

## Harness blitter seam fix

While investigating a pre-existing DiagROM2 harness artifact with periodic
vertical faults inside blitted shapes, the strongest code-level suspicion was
confirmed in the blitter copy path.

### Symptom

The visible artifact looked like:

- regular vertical faults inside blitted colored shapes
- stable global raster/grid outside those shapes

This suggested a blitter source-word continuity problem rather than a Denise
or framebuffer-wide column error.

### Root cause

In:

- `src/chipset/agnus/blitter.c`

the copy-mode path was:

- masking fetched A words with `BLTAFWM/BLTALWM`
- then using the masked value as `aold` for the next word's barrel-shift carry

That is structurally wrong for multiword continuity.

The previous code effectively let first/last-word masks corrupt the source-word
history used for the next shifted word.

### Fix

The copy-mode path now:

1. fetches the raw A word
2. uses the raw previous/current word pair in `blitter_barrel_shift(...)`
3. applies the first/last-word mask to the shifted `aval`
4. preserves the raw fetched word as the next `aold`

This keeps edge masking local to the current destination word instead of
poisoning cross-word continuity.

### Regression test

Added a harness integration test in:

- `tests/integration/test_machine_overlay.c`

New test:

- `test_agnus_blitter_copy_preserves_word_continuity_with_first_word_mask()`

This test programs:

- a two-word source
- an A-only copy blit with shift
- a first-word mask that clears the low nibble

and verifies that:

- the first destination word reflects the current mask
- the second destination word still keeps continuity from the raw previous
  source word

### Related maintenance

The blitter integration tests were also updated to explicitly enable:

- `DMAF_DMAEN`
- `DMAF_BLTEN`

before starting the blitter, to match the newer DMA-arbiter-driven execution
model.

### Validation

Validated with:

- `cmake -S tools/harness -B out/harness`
- `cmake --build out/harness`
- `ctest --test-dir out/harness --output-on-failure -R bellatrix_integration_overlay`

The integration test suite passed after the fix.

## Harness line-mode blitter address fix

Further investigation of the remaining DiagROM2 harness artifact showed that
the affected drawing path was not the copy blitter path above, but the
line-mode fallback path.

### Signal from runtime logs

The important repeated trace pattern was:

- `[BLITTER] start ... con1=0011 ...`
- `[BLITTER] line mode fallback`

So the problematic screen was being produced by:

- `blitter_execute_line_fallback(...)`

not by `blitter_execute_copy(...)`.

### Root cause

In:

- `src/chipset/agnus/blitter.c`

the line-mode fallback was computing the destination word address with:

- `offset >> 3`

This advances the backing address every 8 pixels, which is wrong for a
16-pixel planar word and can also produce odd-byte `read16/write16` access
patterns.

For bitplane words, the address must advance on 16-pixel boundaries, i.e. by:

- one 16-bit word every 16 pixels

### Fix

Added a helper:

- `blitter_line_word_offset(int offset)`

and changed the line-mode fallback to use 16-pixel word addressing:

- `((offset >> 4) << 1)`

The reverse-direction octants were also updated so they stay on even word
addresses instead of relying on `plane_addr + 1` byte-skew tricks.

### Regression test

Added a new harness integration test:

- `test_agnus_blitter_line_mode_crosses_16_pixel_word_boundary()`

This test draws a 17-pixel horizontal line in line mode and verifies:

- the first 16 pixels fill the first destination word (`0xFFFF`)
- the 17th pixel lands in the next word (`0x8000`)

This directly protects against the old 8-pixel/odd-byte seam bug.

### Validation

Validated with:

- `cmake --build out/harness`
- `ctest --test-dir out/harness --output-on-failure -R bellatrix_integration_overlay`

The integration test passed after the line-mode address fix.

## Incremental Paula disk DMA grant flow applied

The next structural cut moved Paula disk DMA transfer visibility behind the
DMA arbiter, while still keeping the existing MFM track preparation logic.

### Previous state

Disk DMA previously did all of this in `paula_disk_start_dma(...)`:

- encode the whole track to MFM
- memcpy the requested bytes directly into chip RAM immediately
- mark DMA active
- wait for a fake countdown in `paula_disk_step(...)`
- only then fire `DSKBLK`

That meant disk transfer visibility was not competing with other DMA clients at
all.

### What changed

Files:

- `src/chipset/paula/paula_disk.h`
- `src/chipset/paula/paula_disk.c`
- `src/chipset/agnus/agnus.c`
- `tests/integration/test_machine_overlay.c`

The new model is:

1. `paula_disk_start_dma(...)` still prepares the MFM track buffer and sync
   state.
2. But it no longer bulk-copies the transfer payload into chip RAM.
3. The payload is now held in an internal prepared track buffer.
4. New helpers expose live DMA demand/service:
   - `paula_disk_dma_wants_service(...)`
   - `paula_disk_dma_service_grant(...)`
5. The Agnus DMA arbiter now:
   - includes a disk request when `DSKEN` is enabled and Paula disk DMA wants
     service
   - services `AGNUS_DMA_REQ_DISK` by transferring the next disk word

### Grant granularity

Disk transfer now becomes visible in chip RAM:

- one 16-bit word per DMA grant

The current cut still uses the already-prepared MFM track buffer, so it is not
yet a rotating-bitcell disk model, but it is materially closer to the real DMA
ownership model because transfer visibility now depends on grant arbitration.

### What became obsolete

`paula_disk_step(...)` no longer owns the actual read transfer completion path.

The fake countdown-based completion path was removed as the authoritative model.

### Test alignment

The disk DMA integration test now explicitly enables:

- `DMAF_DMAEN`
- `DMAF_DSKEN`

before starting disk DMA, and completion is verified after:

- `32` machine ticks / grants for a 32-word transfer

instead of after a fake `46000`-cycle countdown.

This aligns the test with the new arbiter-driven model.

### Validation

Validated with:

- `cmake --build out/harness`
- `ctest --test-dir out/harness --output-on-failure -R bellatrix_integration_overlay`

The harness integration test passed after the disk DMA integration.

## Live Paula audio wiring restored

The next cleanup/integration step focused on Paula audio.

### Important finding

The `paula_audio.*` module already existed, but it was effectively outside the
live Bellatrix path:

- `Paula` did not embed a `PaulaAudio` instance
- Paula register dispatch did not route `AUDx*` writes
- `paula_audio.c` was not even part of the harness or Emu68 Bellatrix target
  source lists
- `src/runtime/core_audio.c` was calling `paula_audio_step(...)` with the wrong
  object type

This meant the project had a partial/stranded audio implementation rather than
an actually live subsystem.

### What changed

Files:

- `src/chipset/paula/paula.h`
- `src/chipset/paula/paula.c`
- `src/chipset/agnus/agnus.c`
- `src/runtime/core_audio.c`
- `tools/harness/CMakeLists.txt`
- `emu68/CMakeLists.txt`
- `tests/integration/test_machine_overlay.c`

Changes:

1. `Paula` now embeds a live `PaulaAudio audio`.
2. `paula_init()` / `paula_reset()` now initialize and preserve audio wiring.
3. Paula now routes `AUD0..AUD3` register writes (`LCH/LCL/LEN/PER/VOL/DAT`)
   into the audio module.
4. `paula_step()` now advances the live Paula audio block.
5. `DMACON` writes in Agnus now synchronize the audio module via:
   - `paula_audio_set_dmacon(...)`
6. `runtime/core_audio.c` now calls:
   - `&core->machine->paula.audio`
   instead of the wrong parent object
7. `paula_audio.c` was added to the actual harness and Emu68 Bellatrix build
   target source lists.

### Validation

Added/updated integration coverage in:

- `tests/integration/test_machine_overlay.c`

New audio test:

- `test_paula_audio_channel0_irq_real_path()`

This verifies that:

- `AUD0*` registers are live
- `DMACON DMAEN|AUD0EN` enables the channel
- a short sample period latches `PAULA_INT_AUD0`
- the resulting interrupt publishes IPL 4

Validated with:

- `cmake --build out/harness`
- `ctest --test-dir out/harness --output-on-failure -R bellatrix_integration_overlay`
- `cmake --build emu68/build-bellatrix-codex`

Both the harness integration test and the Emu68 Bellatrix build passed.

### Cleanup value

This was also a meaningful cleanup of the older model:

- the audio module is no longer a dead side implementation
- the core audio wrapper no longer points at the wrong type
- live targets now actually link the audio implementation they reference

## Sprite DMA brought into the shared Denise model

The next step moved sprite state/fetch closer to the shared graphics model that
the future `core_gfx` path is supposed to own.

### Important correction

An initial read treated `src/runtime/core_gx.c` as effectively legacy because
the current harness/Emu68 live path renders through `agnus_step()` +
`denise_render_line()`.

That was the wrong architectural conclusion.

`core_gfx` is part of the intended refactor for a dedicated graphics core, so
the sprite work should strengthen that shared model rather than route around it.

Because of that, the sprite cut intentionally focused on:

- shared `Denise` sprite state
- real `SPRx*` register handling
- Agnus DMA grants feeding Denise sprite words

and did **not** try to force a one-off sprite compositor into the current line
renderer.

### What changed

Files:

- `src/chipset/denise/denise.h`
- `src/chipset/denise/denise.c`
- `src/chipset/denise/sprites.h`
- `src/chipset/denise/sprites.c`
- `src/chipset/agnus/agnus.c`
- `tools/harness/CMakeLists.txt`
- `emu68/CMakeLists.txt`
- `tests/integration/test_machine_overlay.c`

Changes:

1. `Denise` now embeds a live `DeniseSprites sprites`.
2. `denise_init()` / `denise_reset()` now initialize/reset the shared sprite
   block.
3. `denise_write_reg()` now routes:
   - `SPRxPTH/PTL`
   - `SPRxPOS/CTL/DATA/DATB`
   into the sprite module.
4. The sprite module gained a real per-line DMA handshake:
   - `denise_sprites_dma_request_mask()`
   - `denise_sprite_dma_service()`
5. `denise_sprite_begin_line()` no longer just marks vertical visibility; it
   now prepares a line fetch contract:
   - outside the active vertical span: sprite line state is cleared
   - inside the active vertical span with a pointer: 2 DMA words are requested
   - with direct `DATA/DATB` but no pointer: the sprite remains usable without
     DMA
6. `agnus_step()` now calls `denise_sprite_begin_line(...)` when the beam enters
   a new raster line.
7. The Agnus DMA arbiter now emits `AGNUS_DMA_REQ_SPRITE0..7` from the live
   path and services them by reading chip RAM and feeding the fetched words into
   Denise sprite state.
8. `src/chipset/denise/sprites.c` was added to both the harness and Emu68
   Bellatrix build target source lists.

### Validation

Added integration coverage in:

- `tests/integration/test_machine_overlay.c`

New sprite test:

- `test_agnus_sprite_dma_fetch_real_path()`

This verifies that:

- `DMACON DMAEN|SPREN` enables sprite DMA requests
- `SPRxPTH/PTL/POS/CTL` are live through the current machine bus path
- one raster line advances far enough for sprite DMA grants to occur
- the fetched A/B words land in `DeniseSprites`
- the sprite pointer advances by one line payload
- the resulting sprite becomes visible through `denise_sprites_pixel(...)`

Validated with:

- `cmake --build out/harness`
- `ctest --test-dir out/harness --output-on-failure -R bellatrix_integration_overlay`
- `cmake --build emu68/build-bellatrix-codex`

All three passed.

### Remaining gap

This cut intentionally stops at shared state + DMA fetch.

What still remains for a fuller sprite implementation:

- compose sprite pixels into the current `denise_render_line()` path, or
  alternatively move more of the final pixel path toward the future `core_gfx`
  ownership model
- attached sprite pair semantics
- sprite/playfield and sprite/sprite collision registers
- stronger scheduling alignment between the current line renderer and the
  future per-pixel `core_gfx` design
