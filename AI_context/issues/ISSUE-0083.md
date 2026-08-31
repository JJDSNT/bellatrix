---
id: ISSUE-0083
title: "The Amiga picture is composed but never raised onto the panel"
status: open
priority: high
type: defect
owner: unassigned
created_at: 2026-08-31
updated_at: 2026-08-31
tags:
  - rigel
  - amigavideo
  - display
blockers: []
related_files:
  - external/aros/arch/m68k-amiga/hidd/amigavideo/amigavideo_chipset.c
  - src/amiga/frame.c
  - src/amiga/bus.c
---

# How it presents

With ISSUE-0082 fixed, DPaint's PAL screen opens without crashing and the
display goes **black, with no mouse pointer**.

Everything on the AROS side completes:

```text
[AmigaVideo:Compositor] BitMapStackChanged: display = 320x256
[AmigaVideo:Compositor]   -- ViewPort->DspIns    = 0x044f39d4
[AmigaVideo:Compositor]   --         ->CopLStart = 0x0000158c
[AmigaVideo] setmode: res 0 fmode 0 depth 5 maxplanes 3 aga 0 agae 0
[AmigaVideo] setmode:  mode 00021000 (320x256x5) bpr=40 fu=3
[AmigaVideo] setbitmap: bm=69ef394 mode=00021000 w=320 h=256 d=5 bpr=40
[AmigaVideo] setcoppercolors: copper colors set
[AmigaVideo:Compositor] Notifying DisplayChange 320x256
```

The copper list is at `$0000158c` -- real chip RAM, so the copper allocation is
sound and the "unchecked AllocMem" candidate from ISSUE-0082 is cleared for
this path.

And the chipset composes nothing:

```text
[BELLATRIX:RIGEL:CENSUS] frame=  89 256x256 ... non-bg=0/1369 sum=54a363aa
[BELLATRIX:RIGEL:CENSUS] frame=5000 256x256 ... non-bg=0/1369 sum=54a363aa
```

The same checksum from frame 89 to frame 5000, across the whole session
including the screen opening. Not one sampled pixel differs from the
background. Rigel is running -- VBLANKs count, the clock is armed -- and its
picture never changes.

# What is not the explanation

- **Not the mode.** `setmode` computed a valid OCS 320x256x5, fetch unit 3,
  depth within maxplanes, and did not bail.
- **Not the copper list.** It is in chip RAM at `$158c`, built, and its tail
  and scroll were set.
- **Not register delivery.** amigavideo writes `$dff000`, which is a
  MACHINE_REGION_EXTERNAL aperture owned by `amiga_bus_ops`, so those writes
  reach Rigel by construction.

# CONFIRMED: the master enable was it, and the picture exists

With `DMACON = SETCLR|DMAEN` set at bring-up:

```text
[BELLATRIX:RIGEL] clock armed by a write to $00dff096
[BELLATRIX:RIGEL] DMA master enable set (no Kickstart does it here)
...
[BELLATRIX:RIGEL:CENSUS] something is drawing
[BELLATRIX:RIGEL:CENSUS] frame=6000 352x256 pitch=4096 bg=00000000
                         non-bg=138/1887 sum=479ec06a flags=0c
```

`flags=0c` is `COPPER_ACTIVE | SPRITES_ACTIVE`. The Copper runs, sprites are
armed, the frame resized itself from the idle 256x256 to a real **352x256**,
and 138 of 1887 sampled pixels carry content. The chipset is composing a
picture for the first time.

# What is left is presentation, not composition

The panel still shows black, and the reason is in the same log:

```text
[BELLATRIX]   $01000000-$011fffff DIRECT   Denise frame aperture (host $31237000)
[VC4HVS] takeover: ACTIVE - list 3584, out 1920x1080, fb 1920x1080 -> 1920x1080
[VideoCoreGfx] assembling off-scanout (front page 0x3d827000, ...)
```

The scanout is vcgfx's framebuffer at `0x3d827000`. Rigel's composed frame is
at host `0x31237000` and reaches the panel only as an **overlay plane on the
VC4 scaler** -- `DeniseView` sets vcgfx's `aoHidd_VC4BM_Overlay` to it and
holds it up for as long as the command runs (`c/DeniseView.c:160`).

So when Intuition activates the amigavideo monitor, vcgfx stops drawing its own
bitmap and nothing raises the overlay. Both halves are working and neither is
on screen.

This is the coexistence question from ISSUE-0081 arriving at its concrete form:
the Amiga picture is a plane, the raise is manual by deliberate choice, and the
moment Intuition activates the Amiga monitor is exactly the moment the plane
would have to go up if it were not.

# Works today

From a Shell, before starting the application:

```text
Run DeniseView SHOW
```

then open the Amiga-mode screen. The plane stays up while DeniseView runs.

# FOUND: the DMA master enable is never set

Two eliminations and one positive, all from the log already captured.

**The bitplanes are in chip RAM.** `canblit()`
(`amigavideo_blitter.c:35`) returns FALSE if any plane is outside chip memory,
and `blit_fillrect()` returns before its trace when it does. The trace is in
the log, so the planes passed. That candidate is dead without a run.

**The Copper never executed a single MOVE.** `rigel_frame_t.flags` carries
`RIGEL_FRAME_COPPER_ACTIVE` -- "Copper executed at least one MOVE this frame"
-- and the census prints it. Every frame of the session says `flags=00`.

**Nothing sets DMACON bit 9.** It gates every other channel: with it clear the
Copper does not run, no bitplane is fetched and the blitter moves nothing, no
matter what the per-channel bits say. Every DMACON write in the whole AROS tree:

```text
amigavideo initcustom      0x80E0   SETCLR|COPEN|BLTEN|SPREN
amigavideo compositor      0x8100   SETCLR|BPLEN
amigavideo (clear)         0x0100   BPLEN off
trackdisk / disk           0x8010   SETCLR|DSKEN
audio                      0x8000 | hwmask
```

Not one of them sets DMAEN. They all assume it is already on, because on an
Amiga the Kickstart left it on long before graphics.library ran. **This machine
has no Kickstart**, so that assumption had nobody to satisfy it.

It was accidentally satisfied for a while: `amiga_bus_display_selftest()` wrote
`0x8380` (SETCLR|DMAEN|BPLEN|COPEN) on its way to drawing a test pattern. The
selftest was switched off at the user's request on 2026-08-31, and the chipset
went quiet with it. Everything since -- the screen that opens correctly and
composes nothing -- follows from that one bit.

# The fix

`amiga_chipset_enable_dma()` in `src/amiga/bus.c` sets `DMACON = SETCLR|DMAEN`
during Rigel bring-up, alone, as the one thing a Kickstart would have left
behind. Every channel stays off until its own owner turns it on, which is what
the rest of the system already expects.

A side effect worth knowing: that write now arms the chipset clock, so the boot
log says `clock armed by a write to $00dff096` where it used to say
`$00bfed01`. Earlier, and by the register that means it.

# What is verified and what is not

Verified under QEMU: the line prints, the boot is healthy, no exception, no
regression.

**Not verified**: that the Copper then runs and the screen shows something.
That needs an Amiga screen to open, and `amigavideo` is a module on the card
rather than in the kernel ELF, so a QEMU boot has to reach Intuition -- which
it does not do inside a short run with this much tracing on. The evidence for
the fix is the register semantics and the fact that the symptom began exactly
when the only writer of that bit was removed; the confirmation is a run on
hardware.

# Where to start

Two candidates, in order of cheapness.

**Where the bitplanes are.** RESOLVED above -- they are in chip RAM.
Kept for the reasoning.

**(superseded)** `bm=0x069ef394` is the driver's instance data in
fast RAM, which is correct, but nothing in the log says where the *planes*
are. They come from the planarbm superclass, and if they are not in chip RAM
then Rigel cannot fetch them: the chip bus masks to 21 bits, so a fast-RAM
plane pointer aliases to arbitrary chip addresses or nothing. Every earlier
`blit_copybox` in this issue's history carried fast-RAM addresses on both
sides, which is suggestive but not conclusive -- those are `struct BitMap *`,
not plane pointers. Print the plane addresses and the BPLxPT words the copper
list actually holds.

**Whether bitplane DMA is on.** `DMACON` and `BPLCON0` reach Rigel through the
custom aperture, but nothing has confirmed Rigel acts on them for this screen.
Rigel's own tracing (`--trace` in the harness, and the driver-side equivalent)
distinguishes "never programmed" from "programmed and not fetched".

The published frame is 256x256 (`[BELLATRIX:RIGEL:FRAME] publishing 256x256
pitch=4096 at $01000000`) while the screen is 320x256. Whether the aperture is
meant to resize with the mode is a third question, and it is downstream of the
first two: a frame that composes nothing would be black at any size.

# The missing pointer, which may share a cause

`data->pointer` on the monitor node is NULL, and Intuition sets it only when
`HIDD_Display_SetCursorShape` returns TRUE (`monitorclass.c:1980`). So
amigavideo refused the cursor shape. No cursor is the visible half of that;
whether it shares a cause with the black screen is unknown.

See ISSUE-0082 for how this was reached.

# The legacy tree had a switch, and it is a different shape

Asked whether the legacy VideoCore driver had an RTG/Denise switch, I said no
after searching `src/`. That was wrong: it is in the patch series, and not
against Emu68 but against **Emu68-tools' `vc4.card`**, the RTG board driver --
`patches/0037-videocore-bellatrix-native-hvs-switch.patch`.

`vc4.card` already carried an **RTG <-> Unicam (CSI)** switch: on a PiStorm the
CSI plane shows the Amiga's own video captured through the camera port, and
Picasso96's `SetSwitch()` flips between the RTG desktop and it. Legacy swapped
the Unicam display list for one pointing at Bellatrix's framebuffer:

```c
/* Bellatrix has no physical Denise/CSI capture.  Its Rigel framebuffer is
   published by Emu68 and can be fed to the same HVS native/RTG switch. */
```

The address arrives as a device-tree property (`/emu68/bellatrix-native-fb`,
patch 0036) and `SetSwitch()` writes the native display list into DISPLIST1 at
`0xf2400024`. Full screen, `CONTROL_UNITY`, `RGBFB_R5G6B5PC`, no scaler.

Against what this tree now ships:

| | legacy | vcgfx_denise.c |
|---|---|---|
| form | whole-screen switch | plane composited above the desktop |
| driver | `SetSwitch()`, the RTG API | a task polling COPPER_ACTIVE |
| scale | unity | 3x integer |
| lives in | `vc4.card` (Emu68-tools) | `vcgfx.hidd` (ours) |

And one difference that changes the whole accounting: in legacy, Bellatrix drew
**straight into Emu68's own framebuffer** -- the one `init_display()` returns --
with no aperture and no copy. Here Rigel composes into a buffer of its own, we
copy each frame into the aperture at `$01000000`, and vcgfx composites from
there. `src/amiga/frame.c` argues the copy is what removes the coherency
question; legacy's arrangement did not have the question to remove.

The switch is not reusable as it stands, because it lives in a driver this tree
does not use: our display is `vcgfx.hidd`. But the mechanism transfers whole --
vcgfx already authors HVS display lists, so a second list plus a DISPLIST1 flip
is the same idea in our own driver, and it would replace the polling task with
something the graphics system drives.

Worth doing, and not yet done. The overlay shipped first because it reuses
`vc4_hvs_overlay()`, which already existed for windowed GL.
