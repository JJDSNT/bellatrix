---
id: ISSUE-0083
title: "The Amiga screen opens and stays black: Rigel composes nothing"
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
