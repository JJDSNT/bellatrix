// AI_context/memory/rigel_ks20_video_investigation.md

# Rigel KS20 Video Investigation

## Scope

This note tracks the Rigel-only investigation for `src/roms/KS20.rom` boot-screen
rendering. This is not an AROS issue and should not use the legacy chipset path
as the implementation target.

## Reference

Expected Kickstart 2.0 boot screen shape:

- purple background
- Amiga checkmark/logo on the left
- Kickstart 2.0 ROM text below the logo
- floppy icon on the right
- no horizontal wrap and no black vertical bar

The user described the original failure in `KS20.jpeg` as a horizontal wrap:
the logical screen began around 30% of the width, rendered through 100%, then
continued with the missing 0%-30% segment on the left.

## Fixed Locally

Changes are in `external/rigel`:

- `src/chipset/denise/video/display_window.c`
  - Hires display windows now scale both `visible_x_start` and
    `visible_x_stop`.
  - Before this, width was doubled for hires but the start coordinate remained
    in lores units, so `rigel_get_frame()` exported the wrong horizontal slice.

- `src/chipset/denise/render/compositor.c`
  - Hires DDF origin now uses the same horizontal scale as the visible window.
  - Sprite composition now honors each sprite's `VSTART/VSTOP`; previously an
    armed sprite could render on unrelated lines and create a vertical black bar.
  - Sprite horizontal composition now scales lores sprite bits across 2 hires
    pixels when `BPLCON0.HIRES` is set. This makes the floppy/sprite-width path
    match the hires playfield coordinate space instead of rendering too narrow.

- `src/chipset/agnus/timing/slot_scheduler.c`
  - `BPL1MOD/BPL2MOD` are now applied only after a scanline that actually fetched
    bitplane DMA.
  - `KS20.rom` uses negative modulos (`0xfffa`). Applying those modulos on lines
    before the visible bitplane fetches shifts pointers and recreates the wrap.

## Verification

Commands used:

```sh
rtk cmake --build out/harness-rigel --target harness -j2
rtk env BELLATRIX_CHIPSET_BACKEND=rigel BELLATRIX_RIGEL_TRACE=1 \
  BELLATRIX_RIGEL_DUMP_FRAME=465 \
  BELLATRIX_RIGEL_DUMP_PPM=/tmp/ks20_465_spritefix.ppm \
  ./out/harness-rigel/harness src/roms/KS20.rom --frames 470
rtk convert /tmp/ks20_465_spritefix.ppm /tmp/ks20_465_spritefix.png
```

Observed after the fixes:

- The horizontal wrap is gone.
- The black vertical bar is gone.
- The floppy/sprite overlay is wider and closer to the Kickstart reference in
  hires mode.
- Frame dump is still `560x145`, matching Rigel's current ECS/hires DIW decode
  for this ROM sequence.

## Session 2026-06-29 — Rendering Pipeline Correctness Fixes

Targeting KS20 improvement while preserving 1943 and EON correctness.

Changes in `external/rigel` (rigel commits `357c4ec`, `67e82ab`):

**BPLCON latch per scanline** (`denise_state.h`, `slot_scheduler.c/h`, `framebuffer.c`, `compositor.c`):
- New fields `line_bplcon0/1/2` + `line_bplcon_valid` in `rigel_denise_output_state_t`.
- Captured at the first bitplane DMA slot on each scanline; reset at line start.
- Compositor uses latched values, not live registers. Fixes frames where the
  copper rewrites BPLCON after DMA has already started for that line.

**Per-line bitplane depth** (`slot_scheduler.c/h`):
- `bitplane_line_depth` captures the actual DMA depth at dispatch time.
- End-of-line pointer advance and `BPL1MOD/BPL2MOD` application use this
  captured depth, not the current `sched->depth`. Prevents mis-advance when
  the copper changes `BPLCON0` mid-frame after DMA already ran.

**DDF window clipping** (`compositor.c`):
- Sprite and HAM render loops now clamp `screen_x` to `[x_start, x_stop]`.
- Pixels outside the active DDF fetch window are not written to the scanline
  buffer. Previously only bounds-checked against the full scanline array.

**Border fill on non-DMA scanlines** (`compositor.c`):
- `clear_scanline_to_border()` fills the scanline with `COLOR00` on lines
  that had no bitplane DMA. Prevents stale pixel carry-over from prior frames.

**VBL sprite reset** (`slot_scheduler.c`):
- `denise_sprites_reset()` called at VBL start each field.
- Ensures armed/vstart/vstop state from the previous frame cannot bleed into
  the new frame before the ROM's sprite DMA initializes the channels.

**Display window offset** (`framebuffer.c`, `rigel_denise_api.c`):
- Visible window left offset corrected to `-128` lores pixels (was `-32`).
- `rigel_get_frame()` applies a minimum height guard: if the decoded DIW
  height is < 64 lines and y0 < 64, y1 is set to at least 256. For normal
  screens, y0 is clamped to 44 and y1 to at least 244.

**CIA /DSKCHG fix** (`rigel_cia_api.c`, `floppy_drive.c`):
- `/DSKCHG` is open-drain: OR'd from all drives independently of selection.
- Not-connected drives no longer assert disk-changed at power-on.
- Drive ID scan correctly overrides /DSKCHG only for the selected drive.

Verified: KS20 improved; 1943 and EON not regressed.

## Remaining Issue

The KS20 screen is still not visually complete:

- logo and floppy are mostly outline/partial bitplane data
- ROM copyright text is missing or not rendered visibly
- fills and some colors do not match the reference
- the visible image appears static across the tested boot-screen frames:
  `/tmp/ks20_850.ppm`, `/tmp/ks20_890.ppm`, and
  `/tmp/ks20_900_sprite_hscale.ppm` compared with ImageMagick `AE=0`

Most likely next areas:

- animation source:
  - Copper trace shows `SPR0PTH..SPR7PTL` are reloaded every frame to
    `0x000490`, which looks like a shared/null sprite list rather than the
    animated floppy artwork.
  - This makes the remaining floppy animation more likely to be bitplane/Copper
    or blitter-driven than hardware-sprite driven.
- bitplane fetch/plane-word count for KS20 hires settings:
  - `BPLCON0=b302`
  - `BPLCON1=0044`
  - `DIW=6395/f4ad`
  - `DDF=0040/00d0`
  - `BPL1MOD=BPL2MOD=fffa`
- BPLCON1 scroll semantics in hires, especially separate PF1/PF2 nibbles.
- Hires planar expansion: compositor currently emits one pixel per bitplane bit
  block position. Verify whether hires needs different source-to-output mapping
  rather than only scaled DIW/DDF coordinates.

## Session 2026-07-02 — KS20 Text Opcode Trace

Added a generic harness diagnostic in `tools/harness/musashi_backend.c`:

- `HARNESS_ROM_WATCH_RANGE1=lo:hi`
- `HARNESS_ROM_WATCH_RANGE2=lo:hi`

It logs `[WATCH-ROM-R]` for reads from watched ROM ranges, including PC and
D/A registers. This is necessary because the text block at `0xFCECC4` is data,
not code; `HARNESS_TRACE_PC_RANGE=0xFCECC4:...` will not fire.

Findings from:

```sh
rtk env BELLATRIX_CHIPSET_BACKEND=rigel \
  HARNESS_ROM_WATCH_RANGE1=0xFCECC4:0xFCED24 \
  ./out/harness-rigel/harness src/roms/KS20.rom --frames 900
```

- `0xFCECC4` is the KS20 copyright-text script/data:
  - `2.0 Roms`
  - `Copyright ...`
  - `Commodore-Amiga, Inc.`
  - `All Rights Reserved`
- The script is first copied/read as longwords by reset code at `PC=0xF800E4`.
- Later it is interpreted by code around `PC=0xFCE716..0xFCE75E`.
- Script opcode `0xFB` is definitely reached:
  - `addr=FCECD2 val=FB` at `PC=FCE71A`
  - operand `0x16` at `PC=FCE71E`
- Text characters then go through the glyph/text routine around `PC=0xFA470C`
  and onward.

Additional run:

```sh
rtk env BELLATRIX_CHIPSET_BACKEND=rigel \
  HARNESS_ROM_WATCH_RANGE1=0xFCECD2:0xFCECD3 \
  HARNESS_WATCH_RANGE1=0x00D700:0x00D900 \
  ./out/harness-rigel/harness src/roms/KS20.rom --frames 900
```

showed many non-zero glyph writes to chip RAM around `0x00D73E..0x00D85x`,
mostly from `PC=0xFA4832` and `PC=0xFA4844`. Therefore the missing visible text
is no longer explained by the script interpreter aborting before glyph draw.
The glyph data is being generated in chip RAM.

Updated working hypothesis:

- The failure is downstream of text generation:
  - the written glyph buffer is not part of the bitplane DMA region being
    fetched for the visible KS20 screen, or
  - Rigel fetch/compositor/window/scroll handling maps that buffer outside the
    exported visible frame, or
  - the data is overwritten/cleared before the frame dump.
- Natural next trace: refine `RIGEL_BPL_FETCH_PROBE` so it can filter by frame
  and address range, then check whether Agnus ever fetches the glyph-written
  range (`0x00D73x..`) when the KS20 text should be visible.

## Session 2026-07-02 — Boot Timing, DIWHIGH, and Remaining Text Failure

User constraint: the KS20 insert-disk screen, including text and floppy
animation, should settle before 400 frames. The earlier trace that reached the
text script only around 900 frames was therefore a symptom, not acceptable
behavior.

### Floppy/default drive model

Important finding: Rigel was modelling all four floppy drives as connected empty
drives at reset. KS20 spends time probing non-DF0 drives, which delayed reaching
the insert-disk/text path.

Local change in `external/rigel`:

- `FloppyDrive` now has `connected`.
- Reset defaults only DF0 connected; DF1-DF3 are disconnected.
- `floppy_insert()` connects the target drive.
- Disconnected drives release lines instead of behaving as empty selected
  drives:
  - `/DSKCHG` high
  - `/WPRO` high
  - `/TRK0` inactive
  - `/RDY` inactive
  - ID bit high
- CIA-B floppy routing ignores select/motor for disconnected drives.
- Public `rigel_floppy_get_status()` now reports disconnected drives as not
  selected.

Verification after this change:

```sh
rtk cmake --build out/harness-rigel --target harness -j2
rtk proxy bash -lc 'BELLATRIX_CHIPSET_BACKEND=rigel \
  HARNESS_ROM_WATCH_RANGE1=0xFCECD2:0xFCECD3 \
  HARNESS_WATCH_RANGE1=0x00D700:0x00D900 \
  ./out/harness-rigel/harness src/roms/KS20.rom --frames 400 2>&1 |
  rg "WATCH-ROM-R|pc=00fa48|WATCH-BPL-RAM-W"'
```

Result:

- `0xFB` script opcode is reached before 400 frames.
- Glyph writes from `PC=0xFA4832/0xFA4844` occur before 400 frames.
- This fixed the "text opcode never runs soon enough" part of the bug.

### ECS DIWHIGH / vertical clipping

KS20 programs:

- `BPLCON0=b302`
- `DIWSTRT=6395`
- `DIWSTOP=f4ad`
- `DIWHIGH=2000`
- `DDF=0040/00d0`
- `BPL1MOD=BPL2MOD=fffa`

Rigel decoded this as a short visible vertical window ending around line 244,
which clipped the lower part of the insert-disk artwork and hid the text region.

Local change in `external/rigel/src/chipset/denise/video/display_window.c`:

- Treat `DIWHIGH=0x2000` with an 8-bit `DIWSTOP` vertical decode as an extended
  vertical stop for this ECS window.
- Clamp `vstop` to `RIGEL_DENISE_MAX_LINES` instead of discarding the geometry
  when the decoded window reaches the PAL raster end.

Observed effect:

- Frame dump size changed from `688x200` to `688x268`.
- Trace now reports `vis=298..858/99..312`.
- The lower screen/artwork region is no longer clipped.

### DIWSTRT=ffff transient horizontal beating

After exposing the lower window, the screen began "batendo horizontalmente".
Trace showed KS20 periodically writes a transient blanking/window value:

- `DIWSTRT=ffff`
- `DIWSTOP=f4ad`

Rigel was accepting that as a real viewport and alternating exported width
between `688` and `476`.

Local change:

- Ignore `DIWSTRT=0xffff` in `display_window_update()` when a valid geometry
  already exists.

Observed effect:

- `RIGEL-FRAME-VIDEO` now remains stable at `688x268`.
- At frame 250, raw registers can still show `diw=ffff/f4ad`, but exported
  visible geometry remains the last valid `6395/f4ad` window.

### Current text status

Text is still not visible. The current visual result is stable horizontally and
shows the extended lower region, but the expected text area renders as blue
striped/incorrect bitplane data or remains blank depending on animation page.

Important traces:

1. Glyph writes are real:

```sh
BELLATRIX_CHIPSET_BACKEND=rigel \
HARNESS_WATCH_RANGE1=0x00D700:0x00D900 \
./out/harness-rigel/harness src/roms/KS20.rom --frames 430
```

Shows many writes to `0x00D73E..0x00D85x` from `PC=0xFA4832/0xFA4844`.

2. Bitplane DMA fetch of the same range can occur while the range is still zero:

```sh
BELLATRIX_CHIPSET_BACKEND=rigel \
RIGEL_BPL_FETCH_TRACE_RANGE=0x00D700:0x00D900 \
RIGEL_BPL_FETCH_TRACE_MIN_FRAME=500 \
RIGEL_BPL_FETCH_TRACE_VFROM=238 \
RIGEL_BPL_FETCH_TRACE_VTO=260 \
RIGEL_BPL_FETCH_TRACE_LIMIT=500 \
./out/harness-rigel/harness src/roms/KS20.rom --frames 620
```

Shows fetches like:

- `frame=570 v=244 plane=2 addr=00d73a data=0000`
- `frame=570 v=244 plane=2 addr=00d73e data=0000`

3. Chip RAM dump at frame 600 confirms the `0x00D700` text buffer is zero by
then:

```sh
BELLATRIX_CHIPSET_BACKEND=rigel \
HARNESS_SCREENSHOT_FRAMES=600 \
HARNESS_SCREENSHOT_DIR=/tmp/ks20_chip \
HARNESS_CHIPDUMP=0xd700:0x200 \
./out/harness-rigel/harness src/roms/KS20.rom --frames 602

od -Ax -tx2 -N 128 /tmp/ks20_chip/chip_600_0d700.bin
```

Output starts with all zero words. Therefore the current failure is not simply
"Rigel DMA cannot see CPU writes"; either the glyph buffer is later cleared or
the ROM alternates/double-buffers pages and Rigel is showing/fetching the wrong
page at the time the text should appear.

### Diagnostic additions currently in tree

`tools/harness/musashi_backend.c`:

- `HARNESS_ROM_WATCH_RANGE1/2=lo:hi`
- Logs `[WATCH-ROM-R]` with PC and selected D/A registers.

`external/rigel/src/chipset/agnus/timing/slot_scheduler.c`:

- `RIGEL_BPL_FETCH_TRACE_RANGE=lo:hi`
- `RIGEL_BPL_FETCH_TRACE_FRAME=N`
- `RIGEL_BPL_FETCH_TRACE_MIN_FRAME=N`
- `RIGEL_BPL_FETCH_TRACE_VFROM=N`
- `RIGEL_BPL_FETCH_TRACE_VTO=N`
- `RIGEL_BPL_FETCH_TRACE_LIMIT=N`
- `RIGEL_BPL_TABLE_TRACE`
- `RIGEL_BPL_DISPATCH_TRACE`

These are env-gated diagnostics and were useful to prove the text path and
bitplane fetch timing.

### Open next steps

- Find who clears or overwrites `0x00D700..0x00D900` after the glyph routine.
  Use `HARNESS_WATCH_RANGE1=0x00D700:0x00D900` and look for later zero writes
  after the `PC=0xFA48xx` glyph writes.
- Track KS20 bitplane pointer page alternation:
  - known pages observed: `006048/0087ee` and `00d73a/00fee0`
  - determine which page should contain final text and whether Rigel advances
    `BPLxPT`/modulos incorrectly.
- Continue comparing with KS31 on 68020 as a validation ROM; user reports the
  same symptom there.
- Revisit ECS register gaps only if traces show KS20 writes them. So far there
  is no evidence of `BPLCON4` or `FMODE` writes in this path; `DIWHIGH` was the
  relevant ECS register found in this session.
