---
id: ISSUE-0072
title: "Bellatrix display output: what the HVS can actually scan out, and what feeding it costs"
status: open
priority: medium
type: research
owner: unassigned
created_at: 2026-08-29
updated_at: 2026-08-29
tags:
  - graphics
  - vcgfx
  - hvs
  - raspberry-pi-3
  - drivers
blockers: []
related_files:
  - aros/arch/m68k-emu68/hidd/vcgfx/vcgfx_hvs.c
  - aros/arch/m68k-emu68/hidd/vcgfx/vcgfx_hvs.h
  - aros/arch/m68k-emu68/hidd/vcgfx/vcgfx_neon.h
  - aros/arch/m68k-emu68/hidd/vcgfx/vcgfx_onbitmap.c
---

# Purpose

Establish what the Raspberry Pi's Hardware Video Scaler can be asked to scan
out, and what it costs to keep a plane fed. **This is not Rigel or Denise work**
-- it is deliberately separated from ISSUE-0073, which decides how Denise
connects to whatever this produces. It is the display twin of ISSUE-0070, and
the split has the same purpose: it can be worked now with no Rigel involved, and
it stops decisions taken for the chipset from degrading the display driver.

The governing principle is the same one:

> Advertise and rely on only what the hardware, the connected sink and the
> Bellatrix implementation actually support.

# What is actually in the tree

`vcgfx` does not use the firmware's framebuffer. It takes ownership of the HDMI
channel and authors its own HVS display list (`vc4_hvs_takeover()`), then
composites: an fb plane, an optional overlay plane, a cursor plane.

**The overlay plane already exists**, built for windowed GL and driven by an
ordinary HIDD attribute rather than a private path:

```c
/* aoHidd_VideoCoreGfxBitMap_Overlay, set on the on-screen bitmap */
struct vc4gfx_overlay
{
    ULONG ovl_Phys;                 /* ARM phys of the pixel data */
    ULONG ovl_Pitch;                /* bytes per row */
    ULONG ovl_Width, ovl_Height;    /* source pixels */
    LONG  ovl_X, ovl_Y;             /* position in fb coordinates */
    ULONG ovl_DestW, ovl_DestH;     /* larger than source = HVS upscale */
};
```

Composited above the fb plane and below the cursor; updates to a live overlay
are patched in place and latch at vblank.

**Pixel formats the HVS understands** (`vcgfx_hvs.h`):

```text
RGB332  RGBA4444  RGB555  RGBA5551  RGB565  RGB888  RGBA6666  RGBA8888
YUV420_3PLANE  YUV420_2PLANE  YUV422_3PLANE  YUV422_2PLANE  H264
```

**There is no indexed/CLUT format in that list.** Anything palette-based has to
be expanded before it reaches a plane. That single fact drives most of
ISSUE-0073.

**Scaling is upscale only** (`Dest != src size = HVS-scaled (upscale only)`),
through a PPF filter kernel copied from the firmware at takeover and gated by
`hvs_KernelOK`. The header carries a warning that authoring a list too long
clobbers `HVS_OWN_KERNEL` and corrupts the filter for a few frames -- the
scaled path is real but it is delicate.

**Vsync pacing exists**: a PV2 vsync interrupt with a frame counter and
`hvs_FlipArmed` for flip pacing.

**The NEON helpers are not what a chipset needs.** `vcgfx_neon.h` has
`neon_copyline`, `neon_copyline_rev`, `neon_blit_mask32_row_opaque` and
`neon_fillline` -- copies, fills and a masked blit. **There is no planar to
chunky conversion and no palette expansion.** Both are new work.

**The HVS does not exist under QEMU**: `vcgfx_hvs.c:1180` reports `no HVS found
(ID=0x%08x) - QEMU or unmapped, skipping`. Everything here is hardware-only,
which is the strongest argument for keeping non-visual verification paths
(serial censuses, checksums) for anything that must be testable in CI.

# Questions to answer

## How many planes, and at what cost

The list currently carries fb + optional overlay + cursor. Determine the
practical ceiling: how many planes can be authored before the list length
becomes the problem the kernel-clobbering comment describes, and what each
additional plane costs in HVS bandwidth at 1080p.

This matters beyond one overlay: sprites, dual playfield and a hardware pointer
are all natural planes, and whether they can *be* planes is a hardware question
that belongs here, not in the chipset issue.

## Which format to feed it

`RGBA8888` is the obvious choice and the most expensive: 4 bytes per pixel of
write bandwidth for content whose source has at most 12 bits of colour.
`RGB555` and `RGB565` halve that, and an Amiga palette entry is 4 bits per gun,
so `RGB555` loses nothing at all for non-HAM content.

Measure the difference rather than assume it. The relevant number is bytes
written per frame by whatever produces the plane, not the HVS's own read cost.

## Scaling quality and aspect

320x256 to a 1080p panel is a 3.4x upscale with non-square source pixels. Decide
what the PPF kernel does to that, whether the result is acceptable, whether
integer scaling with borders is preferable, and how hires/lace source modes
change the answer. Verify `hvs_KernelOK` is set on the target hardware rather
than assumed.

## Coherency

A plane is read by DMA. Anything producing pixels from the ARM side must land in
memory the HVS sees. This is the open work in
`AI_context/consolidated/vc4_memory_coherency_upstream.md` and patch
`emu68/0018-map-vc4-memory-normal-non-cacheable.patch`; the display question and
the audio question meet here, and the answer should be one facility rather than
two.

## Pacing and tearing

An overlay latches at vblank. Determine what happens when a producer updates the
buffer faster or slower than the panel refreshes: whether double buffering is
needed, whether the vsync IRQ counter is the right pacing signal, and what
tearing looks like when a producer runs at a rate unrelated to 50 or 60 Hz --
which is the normal case for anything driven by an emulated machine.

## Sink capabilities

The video counterpart of ISSUE-0070's EDID work. Modes, refresh rates and
whether the sink accepts them. **Do not build a second EDID parser**: that issue
already proposes one Bellatrix facility feeding both video and audio, and
`vcgfx` is the other interested party.

# Validation matrix

```text
[ ] overlay appears and disappears cleanly
[ ] position and size are correct
[ ] upscale is correct at several ratios
[ ] no tearing at the panel's refresh rate
[ ] no corruption of the fb plane or cursor
[ ] the PPF kernel survives repeated list authoring
[ ] coherency holds with a producer writing every frame
[ ] cost measured per format (RGBA8888 / RGB565 / RGB555)
[ ] multiple planes, if supported, do not exceed HVS bandwidth
[ ] long-duration stability
```

# Order

1. Audit what `vc4_hvs_overlay()` supports today on real hardware
2. Measure the cost of feeding one plane, per pixel format
3. Establish the plane ceiling and the list-length limit
4. Settle coherency jointly with the audio side
5. Settle pacing and double buffering
6. Decide scaling policy for non-square, low-resolution sources
7. Add planar-to-chunky and palette expansion to the NEON helpers
   (needed by ISSUE-0073, but it is host-side code and belongs here)
8. EDID-derived video mode validation, sharing ISSUE-0070's facility
