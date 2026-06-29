# Rigel Graphics DMA and Horizontal Scroll Investigation

## Scope

Investigation of Rigel graphics problems observed in:

- `src/roms/KS20.rom`
- `src/disks/1943.adf`
- `src/disks/battle.adf`
- `src/disks/eonA.adf`
- `src/disks/sota.adf` (future reference; different bug category)

The work is Rigel-specific. Do not use the legacy chipset path as the target.

## Harness SDL Change

The harness SDL window is now a stable diagnostic surface:

- Default harness framebuffer/window changed from `640x512` to `768x576`.
- In `BELLATRIX_HARNESS`, `machine_present_frame_from_rigel()` no longer calls
  `PAL_Video_Resize(frame.width, frame.height, 16)`.
- `PAL_Video_Resize()` no longer calls `SDL_SetWindowSize()` in the SDL path.

Reason: the previous presenter resized the host window to every exported Rigel
frame. This made DIW/DDF/viewport bugs harder to inspect because the diagnostic
surface changed with the emulated mode.

## EON Findings

`eonA.adf` reaches a problematic mode around frame 1642 onward:

```text
BPLCON0=6601 depth=6 lores dual-playfield
BPLCON1 varies per frame/line
BPLCON2=0044
DIW=5881/00c1
DDF=0030/00d0
```

Initial screenshot `eon.jpg` showed severe horizontal slicing. This was not a
host SDL issue: the frame export was stable, but bitplane data was composed in
the wrong order.

## Changes Tried

### Dual Playfield Scroll

`external/rigel/src/chipset/denise/render/compositor.c`

- Before: one scroll value, `BPLCON1 & 0x0f`, was used for all playfield pixels.
- After: dual playfield uses separate scroll nibbles:
  - PF1: low nibble
  - PF2: high nibble

This is conceptually required but did not by itself fix EON.

### Bitplane Logical Slot Index

`external/rigel/src/chipset/agnus/timing/slot_scheduler.c/.h`

- Added `bitplane_slot_index[hpos]`.
- Bitplane DMA dispatch derives:
  - `plane = logical_index % depth`
  - `word = logical_index / depth`
- Fetch reads from `line_base[plane] + word * 2`, instead of depending only on
  the mutable live `BPLxPT`.
- End of line advances active bitplane pointers by `words_this_line * 2`, then
  applies `BPL1MOD/BPL2MOD`.

This separates physical DMA timing from logical scanline word order.

### Lores 6-Plane Scheduling

The old lores scheduler treated bitplane DMA as a simple stride-2 stream.
That is not enough for high-depth lores. For `depth=6`, each 16-pixel fetch
group needs six bitplane slots.

Current implementation schedules lores bitplane DMA as:

```text
hpos = DDFSTRT + word * 8 + plane
logical_index = word * depth + plane
```

The EON frame improved substantially after this: `/tmp/eon_1722_lores_groups.png`
shows a coherent planet/image rather than disconnected horizontal stripes.

### Scheduler Regression Test

`external/rigel/tests/test_agnus_domains.c`

- Added matrix coverage for EON-style lores scheduling:
  - depths 1-6
  - `DDFSTRT=0x0030`
  - `DDFSTOP=0x00d0`
- The test asserts physical bitplane slot positions, logical slot indices, and
  end-of-line `BPLxPT` advance by 21 words per active plane.
- The existing protected copper/blitter check was corrected to match current
  `copper_exec_move` semantics: a protected MOVE to a low register stops copper
  until VBL and does not start the blitter.

### BPLCON1 Scroll Tests

`external/rigel/tests/test_denise.c`

- Added synthetic lores single-playfield coverage for `BPLCON1` low-nibble
  scroll.
- Added synthetic lores single-playfield coverage for separate odd/even plane
  scroll when the high nibble differs from the low nibble.
- Added synthetic lores dual-playfield coverage showing PF1 uses the low nibble
  and PF2 uses the high nibble independently.
- These tests pass with the current compositor, so remaining EON issues are less
  likely to be the basic low/high nibble split and more likely to involve hires,
  per-line changes, pipeline positioning, or another subsystem.

## 1943 Trainer Finding

`src/disks/1943.adf` custom boot/trainer reaches the "PIRANHAS" screen by
frame 350/360 in the `KS13.rom + 1943.adf` path. Frame 50 is too early and
still shows the initial boot state.

The visible failure was not the SDL harness size and not primarily BPLCON1
viewport clipping. The trainer changes `BPLCON0` depth several times inside the
frame:

```text
0200 -> 1200 depth=1
1200 -> 4200 depth=4
4200 -> 1200 depth=1
1200 -> 2600 depth=2
2600 -> 0200 depth=0
```

The important transition is `1200 -> 4200` before `DDFSTRT=0038`. Before the
fix, `agnus_slot_scheduler_set_depth()` changed the cached depth but did not
dirty/rebuild the bitplane slot table. A line that should fetch 4 bitplanes
could therefore keep the 1-bitplane slot layout from the earlier split. The
visual symptom was the `P` from `PIRANHAS` appearing wrapped/separated at the
right edge.

Fix:

- `agnus_slot_scheduler_set_depth()` now sets `table_dirty` when depth changes.
- `test_agnus_domains` has a targeted regression for the 1943-style
  `BPLCON0 1200 -> 4200`, `DDFSTRT=0038`, `DDFSTOP=00d0` case.

Validated dump:

```sh
rtk env BELLATRIX_RIGEL_TRACE=1 \
  BELLATRIX_RIGEL_DUMP_FRAME=350 \
  BELLATRIX_RIGEL_DUMP_PPM=/tmp/1943_350_depthdirty.ppm \
  KICKSTART=src/roms/KS13.rom \
  ADF=src/disks/1943.adf \
  FRAMES=360 \
  ./run.sh harness
```

After the fix, `/tmp/1943_350_depthdirty.png` shows `PIRANHAS` complete with
left/right sprites visible.

## Reference Backlog

`src/disks/sota.adf` has been added as a future reference case. `sota.jpg`
captures the current symptom: horizontal lines appear to leak across the image.
It should not be used as a direct visual comparison against EON, KS20, 1943, or
Battle Squadron until its bug category is classified.

## SOTA Finding

`src/disks/sota.adf` has now been reproduced headless at frame 987 with
`KS13.rom`:

```text
BPLCON0=2200 depth=2 lores single playfield
BPLCON1=0020
BPLCON2=0024
DIW=1c71/3ed1
DDF=0030/00d8
DMACON=07ff
export=465x256 vis=113..465/28..284
```

The dumped frame has only two colors, so the horizontal leaks are real playfield
bits, not SDL/window/presenter conversion artifacts.

Important negative results:

- Changing single-playfield `BPLCON1` so even planes used the low nibble did not
  change frame 987.
- Fetching from live `BPLxPT` on each bitplane slot changed the image heavily
  and made it less like `sota.jpg`; keep the current model where a line captures
  `BPLxPT` bases and advances active pointers at end-of-line before modulo.

The Copper/bitplane hypothesis was later disproved. A frame/beam-filtered trace
showed no visible-region `copper_write` activity for frame 987; bitplane fetches
were reading already-written chip RAM. A chip-write watch over a leaking
bitplane region showed line-mode blitter writes placing edge bits, followed by
descending exclusive fill (`BLTCON0/1=09f0/0012`) expanding those bits into the
visible horizontal leaks.

Confirmed fix: SOTA depends on real blitter line-mode semantics. The previous
Rigel path used an approximate octant/Bresenham address model. It has been
aligned with the WinUAE reference behavior: `BLTADAT/BLTAFWM` provides the
shifted A pixel, `BLTBDAT` rotates as the pattern source, `BLTCON1[6]` seeds the
line sign, `BLTCON1[1]` controls single-dot writes, and `BLTCON1` bits drive X/Y
advance. User visual confirmation: the leaked horizontal lines are removed.

## Verification Used

Build and focused tests:

```sh
rtk cmake --build out/harness-rigel --target harness -j2
rtk ctest --test-dir out/harness-rigel \
  -R 'test_(timing|agnus_domains|denise|dualpf|priority|video_modes)' \
  --output-on-failure
```

Latest focused result after expanding scheduler, BPLCON0 depth-change, and
`BPLCON1` tests: all 6 selected tests passed.

EON dump:

```sh
rtk env BELLATRIX_RIGEL_TRACE=1 \
  BELLATRIX_RIGEL_DUMP_FRAME=1722 \
  BELLATRIX_RIGEL_DUMP_PPM=/tmp/eon_1722_lores_groups.ppm \
  KICKSTART=src/roms/KS13.rom \
  ADF=src/disks/eonA.adf \
  FRAMES=1725 \
  ./run.sh harness 2>&1 | \
  rtk grep --line-buffered -E '\[(RUN|HARNESS|RIGEL-DUMP|RIGEL-FRAME-VIDEO)\]'
rtk convert /tmp/eon_1722_lores_groups.ppm /tmp/eon_1722_lores_groups.png
```

## Frame Counter Caveat

There is no single global frame counter in the current harness/Rigel stack.
Do not compare frame numbers from different layers as exact timestamps unless
they are logged at the same trace point.

- The SDL title `frame=...`, `FRAMES=...` stop condition, and scripted input
  use the harness-local counter in `tools/harness/main.c`. It increments when
  `bellatrix_machine_get()->frame_counter` changes.
- `g_machine.frame_counter` is the machine/presenter frame counter updated by
  the Bellatrix machine step path.
- Rigel/Denise frame dumps use the backend video frame counter exposed through
  `rigel_get_frame().frame_count`, sourced from `denise.output.frame_counter`.
  Denise updates this from the Agnus beam frame count when the framebuffer
  observes a new video frame.
- `[RIGEL-FRAME-VIDEO] N=...` uses the Rigel trace layer's own
  `g_rtrace.frame_count`.

Practical consequence observed in EON: the SDL title can show `frame=2619`
while an internal Denise/scheduler trace reports a different frame number for
the same visual moment. For graphics debugging, use one counter consistently
or add a correlation log that prints both harness and Denise/Rigel counters in
the same trace line.

## Remaining Problems

EON is improved but not 100% correct. Frame 2619 now has two separated
failure classes:

- The vertical white rectangle is sprite overlay. Temporarily disabling sprite
  overlay removes it. A focused trace showed sprites `1..5` drawing `pix=3`
  as contiguous 16-pixel blocks from `vstart=124` to `vstop=256`.
- The horizontal lines remain when sprite overlay is disabled, so they are still
  a playfield/blitter/composition issue.

Additional findings:

- The BPL5 memory range visible in frame 2619 is written by a blitter command
  `BLTCON0/1=0d0c/0000`, `dspan=009040..00a504`, `size=12x190`. This explains
  why high-plane data exists, but does not by itself explain the sprite
  rectangle.
- Dual-playfield PF1-vs-PF2 priority must use `BPLCON2.PF2PRI` (bit 6), not
  `PF1P/PF2P`. `PF1P/PF2P` remain sprite priority thresholds. The helper and
  compositor were corrected, and `test_dualpf`, `test_denise`, `test_priority`
  passed. This did not change the EON frame 2619 artifact.

Remaining likely areas:

- `bitplane_words_per_plane()` may still over/under-count for some lores DDF
  ranges and depths.
- The exact OCS/ECS DMA slot layout still needs validation against hardware
  references for non-EON DDF ranges.
- `BPLCON1` semantics still need review for hires, per-line changes, and
  interaction with pipeline positioning. Basic lores single/dual semantics now
  have unit coverage.
- Viewport/export still needs targeted checks for Battle Squadron sprites and
  KS20 hires screens.
- `sota.adf` needs separate classification before it is assigned to a subsystem.
- EON sprite DMA/state needs focused investigation: why sprites `1..5` remain
  active as a solid mask through lines 124..255.

## Next Recommended Target

The highest-value automated target is EON frame 2619 because it reproduces
headlessly and now has an isolated sprite-overlay artifact. Battle Squadron
remains the gameplay-facing sprite/scroll target, but its custom bootloader
requires manual interaction before the bug can be captured. SOTA is confirmed
fixed for the previously observed horizontal-line leak and should remain a
regression reference for blitter line-mode, not a direct comparator for EON.
