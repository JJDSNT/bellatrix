---
id: ISSUE-0073
title: "Rigel Denise to the Bellatrix display: the renderer is a policy, not an architecture"
status: open
priority: medium
type: research
owner: unassigned
created_at: 2026-08-29
updated_at: 2026-08-29
tags:
  - graphics
  - rigel
  - denise
  - hvs
  - integration
blockers:
  - ISSUE-0072
related_files:
  - src/amiga/bus.c
  - external/rigel/include/rigel/rigel_denise_video.h
  - aros/arch/m68k-emu68/hidd/vcgfx/vcgfx_hvs.c
---

# Purpose

How Denise in Rigel should put an image on the Bellatrix display. It is the
display twin of ISSUE-0071 and follows the same rule:

> Rigel owns Denise semantics. Bellatrix/AROS owns the bridge to the display.

**Prerequisite: ISSUE-0072**, for the same deliberate reason the audio pair has
one: that issue makes the host display path understood with no Rigel in it, so
decisions taken for the chipset do not deform the display driver.

# The direction, stated once

Denise is a **source**; the HVS is the **sink**; the bridge is a client of the
display driver, exactly as the audio bridge is a client of AHI.

```text
guest program -> bitplane pointers, BPLCONx, palette, copper -> Rigel's Denise
   -> pixels -> bridge -> an HVS plane -> panel
```

Nothing here asks the display driver to become a chipset, and nothing lets the
display pull on Rigel. `vcgfx` keeps owning the HVS display list.

# Four possible boundaries

**A. Rendered frame.** `rigel_get_frame()` gives finished RGBA for a whole
frame. One plane, one buffer, simplest possible bridge. It costs Rigel's full
per-colour-clock Denise pipeline.

**B. Rendered scanlines.** `rigel_get_scanline()` already exists and its
`rigel_denise_scanline_t` carries `dirty` and `last_rgb32`, so the host can
update only what changed. Cheaper to *copy*; identical cost inside Rigel.

**C. Line-level rendering inside Rigel.** Rigel stops composing per colour clock
and renders each scanline from the register state at its change points. Output
is still pixels; what changes is Rigel's own cost -- and that is where a large
part of the 3.8x lives, since `rigel_denise_framebuffer_sync_from_beam` (13.2%)
and `rigel_denise_compositor_tick` (5.3%) run once per colour clock for a screen
with nothing on it. This is hypothesis 4 of Rigel's own
`from_bellatrix/rigel_performance_research.md`, and it belongs to Rigel's
ISSUE-0006 rather than to this issue.

**D. Parameter hand-off.** Give the host the display description -- bitplane
pointers and modulos, BPLCON0/1/2, the palette, DIWSTRT/STOP, DDFSTRT/STOP,
sprite state -- and let the host build the image.

# The asymmetry with audio, which is the whole point

The audio design's boundary D works because **AHI's mixer is a functional
equivalent of Paula**: it reads 8-bit samples from memory at a rate, scales by
volume, pans and mixes. Hand it the parameters and the host hardware does
Paula's job.

**The HVS is not a functional equivalent of Denise.** It scans out chunky
RGBA/RGB565-family planes and composites and scales them. It has **no indexed
format at all** (ISSUE-0072 lists what it does have), so it cannot read planar
bitplanes, cannot do a palette lookup, cannot do HAM, and cannot do
dual-playfield priority resolution.

So the graphics D is **not** "the host hardware does it for free". The planar to
chunky decode and the palette expansion happen in software either way; D only
moves them from Rigel's per-colour-clock loop to the ARM, where NEON can do them
per line. That is still worth a lot -- and note ISSUE-0072 records that the
existing `vcgfx_neon.h` has copies, fills and a masked blit but **no planar to
chunky and no palette expansion**, so this is new code, not a wiring job.

Where the HVS *is* a functional equivalent, it should be used:

- **scaling** -- 320x256 to the panel, free, instead of a software upscale;
- **sprites** -- natural HVS planes, the cursor already is one;
- **dual playfield** -- plausibly two planes with priority, if the plane ceiling
  in ISSUE-0072 allows;
- **compositing with the AROS desktop** -- what makes the overlay usable during
  bring-up at all.

# The transferable insight: the renderer is a policy

This is what carries over from the audio design unchanged, and it is the useful
part.

- **Rigel is always the authority** for what the guest observes: beam position
  through VPOSR/VHPOSR, copper timing, sprite collisions, bitplane DMA fetch
  timing and slot contention. Every custom-register write reaches it regardless
  of who produces the pixels.
- **The renderer is a policy.** A, B, C and D leave the guest-observable state
  identical, because it comes from Rigel either way. So this is not a fork in
  the architecture; it is which path produces the image, and it can change at
  run time.

And as with audio, **the fallback condition is observable from Rigel's own
state**. Parameter hand-off cannot represent a display whose registers change
within a line -- so the policy is automatic rather than configured:

```text
copper writes BPLCONx / COLORxx mid-line   -> exact path for that line
HAM active                                 -> exact path
dual playfield beyond the plane ceiling    -> exact path
sprite/playfield collisions being read     -> exact path
otherwise                                  -> parameter hand-off
```

`rigel_frame_t` already carries `RIGEL_FRAME_HAM`, `RIGEL_FRAME_DUAL_PLAYFIELD`,
`RIGEL_FRAME_SPRITES_ACTIVE` and `RIGEL_FRAME_COPPER_ACTIVE`, which is most of
that test at frame granularity. Per line it would need something finer, and
that is the Rigel-side ask.

# What the bridge carries

Whichever path, the bridge is a descriptor plus a buffer, and the buffer is not
zero-copy to begin with:

- **A/B/C**: a physical address, pitch, width, height and active flag for a
  non-cacheable copy of the finished frame. ISSUE-0068 has the reasoning --
  Rigel's frame buffer comes from Emu68's cached TLSF heap with no way to
  identify which allocation it is, and one 320 KB copy per frame removes the
  coherency question entirely.
- **D**: the display description per line, plus the guest's bitplane data read
  straight from chip RAM. Chip RAM is mapped direct by construction, so the
  pointer the guest wrote is a pointer the ARM can also read -- the same
  property that makes the audio bridge carry no samples.

# What to ask Rigel for

Nothing, for A and B: `rigel_get_frame()` and `rigel_get_scanline()` already
exist and are enough to put a picture on screen. **Start there.**

For D, Rigel would need to expose the display description per line, and for the
policy above it would need a per-line version of the frame flags. That is
written up as Rigel's ISSUE-0008. It is explicitly *not* a prerequisite: it buys
speed, and it should be asked for only once the exact path is working and
measured.

C is Rigel's own performance work, ISSUE-0006, and needs nothing from us.

# Correction: QEMU is missing the HVS, not a framebuffer

An earlier version of this issue, and of ISSUE-0068, said the display work
lights up only on hardware. That is true of the HVS overlay and false of
everything else -- AROS shows a picture under QEMU, so a linear framebuffer
plainly exists. There are three routes, and only the last needs the HVS:

**1. The ARM side writes into Emu68's framebuffer.** `start_rpi64.c:239` does
`init_display(sz, (void**)&framebuffer, &pitch)` and keeps globals our code can
already use:

```c
extern uint16_t *framebuffer;
extern uint32_t pitch, fb_width, fb_height;
```

No AROS, no HVS, no guest-visible copy, and `run.sh`'s `screendump` captures
it. The catch is ownership: `bootui.c` calls `bootui_retarget()` when `vcgfx`
takes the display -- the `[BootUI] retargeted to RGB32 framebuffer` line in
every boot log -- and from then on AROS is scanning a different buffer while
Emu68's RGB16 pointer is stale. So this route serves a test *before* the
desktop, which is what a first bring-up wants.

**2. An AROS display driver, modelled on our own `fbgfx`.** This port already
has two display drivers and a handover between them, and the first one is
exactly the template:

```c
/* aros/arch/m68k-emu68/hidd/fbgfx/ -- "VideoCore framebuffer graphics HIDD",
 * twelve files: bitmap class, display class, refresh. It gets its surface
 * from the kernel, which got it from Emu68: */
data->width  = KrnGetSystemAttr(KATTR_FrameBufferWidth);
data->height = KrnGetSystemAttr(KATTR_FrameBufferHeight);
/* "5:6:5 in a halfword. Emu68 hands this over on the Raspberry Pi" */
```

So the Emu68-to-AROS surface handover already exists (`start.c:2280` puts
`fb_width` in D1 and the pointer in A0), `fbgfx` starts the machine, and
`vcgfx` takes the display later through `bootui_retarget()`. A third driver
whose surface is Rigel's frame is the smallest new AROS code possible, it is
QEMU-testable because no HVS is involved, and **the source switch comes free**:
AROS's monitor machinery already performs the `fbgfx` to `vcgfx` handover, so a
third monitor is that mechanism rather than a new one.

An earlier draft of this section proposed a plain program blitting into a
window instead. That is more artisanal and less reusable; the driver is the
step that is not thrown away.

**3. The HVS overlay.** Hardware only, but zero-copy, hardware-scaled and
composited above the desktop. From route 2 this is not a different design --
it is the same driver presenting through a plane instead of a copy.

Routes 2 and 3 are the same bridge: a descriptor plus a buffer. Only where the
buffer lives changes (guest-visible RAM against non-cacheable ARM memory).

## The semantic wrinkle in route 2, and what it implies

`fbgfx` composes into each bitmap and copies to the framebuffer -- data flows
from AROS to the screen. For Denise it flows the other way, from Rigel to the
screen. A display driver whose content arrives from outside is a screen AROS
believes it may draw into and must not. That is fine for a diagnostic and wrong
as a final design.

The final design is the other one, and this issue should not lose sight of it:
**`amigavideo` plus a presenter.** With the native chipset display driver built
(ISSUE-0068 step 3), AROS draws *through* the chipset -- bitmaps in chip RAM,
Denise renders them -- and presentation stops being a display driver at all. It
becomes just the component that puts Denise's output on the panel.

The two do not compete. The presenter in the final design is exactly what a
`fbgfx`-shaped Denise driver has to do anyway, so building route 2 builds it.

# Order

1. **A picture, under QEMU, through route 1.** `rigel_get_frame()` on
   `RIGEL_EVENT_FRAME_READY`, blitted into Emu68's framebuffer before the
   desktop takes it, captured with `screendump`. A non-black pixel census on
   the serial line alongside it, since a census is what a headless CI can
   assert on.
2. **Route 2**: copy into guest-visible RAM, publish the descriptor, and
   present it from an AROS display driver modelled on `fbgfx`. Still
   QEMU-testable, it survives the desktop, and it is the component the final
   design needs anyway.
3. **Boundary A through the overlay**: the same descriptor pointing at
   non-cacheable memory, one HVS plane. Hardware only. At this point anything
   writing `$dff000` is visible with the desktop intact.
4. **Boundary B**: use the `dirty` flags to copy only changed lines; measure
   whether it matters.
5. **Measure** where the time actually goes with a real workload before
   choosing between C and D. C is free to us and helps every host; D is ours to
   build and helps only this one.
6. **D, if the measurement justifies it**, with the exact path kept as the
   automatic fallback.


# 2026-08-30: the plane, at last

Everything below the last hop had been working for a while: the frame is in the
aperture at `$01000000`, published every frame, and both sides agree on what is
in it. What was missing was putting it on the panel -- and the consequence was
a machine that renders correctly and looks frozen, which is an expensive way to
be right. Two boots were read as hangs because of it.

`DeniseView SHOW` installs the plane:

```c
struct vc4gfx_overlay desc = { phys, pitch, width, height, 0, 0, destw, desth };
tags[0].ti_Tag = OOP_ObtainAttrBase("hidd.bitmap.bcmvc4") + aoHidd_VC4BM_Overlay;
tags[0].ti_Data = (IPTR)&desc;
OOP_SetAttrs(HIDD_BM_OBJ(screen->RastPort.BitMap), tags);
```

It holds the plane while it runs and takes it down on Ctrl-C, so the chipset's
output sits **above** the desktop rather than instead of it -- which is what
makes it usable for looking at something while the machine still works.

## Two things the Bellatrix side had to add

**The physical address.** The descriptor published the guest address, which is
what the m68k dereferences; the scaler reads by DMA and needs the other one.
`REG_PHYS` at offset `0x20`, version bumped to 2. The two are not
interchangeable, which `machine/region.h` says at length and this session
already paid for once.

**A cache clean after the copy.** Bellatrix writes the aperture through a
cached mapping and the scaler reads it by DMA, so without `arm_flush_cache()`
the plane scans whatever was in that memory before -- a display showing stale
or torn content while every value in the machine is correct. One megabyte per
frame at 50 Hz is 50 MB/s of cache maintenance, which is worth measuring later;
correct first.

## What it should show

Grey. `0x00aaaaaa`, the Workbench grey `amigavideo` programmed into COLOR00 --
which is the whole content of the chipset's frame today, and the first time
anything this chipset produced will have been seen rather than counted.
