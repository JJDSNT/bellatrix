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
