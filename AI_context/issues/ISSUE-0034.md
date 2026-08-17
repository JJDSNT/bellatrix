---
id: ISSUE-0034
title: "Hold the boot UI until the desktop is drawn, as the one exception to the takeover rule"
status: backlog
priority: medium
type: feature
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - bootui
  - graphics
  - emu68gfx
blockers:
related_files:
  - aros/arch/m68k-emu68/hidd/emu68gfx/emu68gfx_hiddclass.c
  - aros/arch/m68k-emu68/boot/bootui.c
  - AI_context/issues/ISSUE-0032.md
---

# Summary

The splash disappears, the screen goes grey and empty, and some seconds later
icons arrive. The gap is the complaint: the boot UI leaves before there is
anything to replace it with.

# The rule this does not change

**The boot UI gives up the display on any screen request.** That is deliberate
and stays. What is wanted here is one narrow exception -- the *first* Workbench
screen during boot -- and nothing else.

Today `Emu68Display__Hidd_Display__Show()` calls `emu68_bootui_takeover()` on
the first `Show()` carrying a bitmap, which is exactly that moment and is also
every other moment.

# Why this is feasible and cheap, which was not obvious

This driver runs AROS's **framebuffer mode**: it owns one linear framebuffer,
the Display class creates a single bitmap pointing straight at it
(`aHidd_ChunkyBM_Buffer`, `emu68gfx_hiddclass.c:179`), and every screen is a
separate bitmap that gets **copied** into that framebuffer when it is shown.

So a screen that has not been shown yet is being rendered somewhere the display
is not. Holding the first `Show()` means Intuition and Wanderer draw the whole
desktop off-screen while the splash stays up, and the desktop then appears
already complete -- no grey phase at all, and no two producers writing the same
pixels. That is a better outcome than merely covering the gap.

# The hard part, which is why this is an issue and not a commit

**The release must be guaranteed.** A held `Show()` that is never released is a
machine that boots to a splash and never shows a desktop, which is far worse
than the gap being fixed. Three candidate signals, none yet chosen:

1. **A helper in `SYS:WBStartup/`.** Workbench launches these after it has
   opened its window, which is the closest thing to "the desktop exists" that
   needs no upstream change. Costs a tiny program on the card.
2. **A call from Wanderer**, added by the patch series, after its first icon
   layout. The most accurate signal and the most invasive.
3. **A watchdog** that releases after a bounded wait regardless. Not a signal at
   all, but the safety net the other two need anyway.

Whatever fires it, the deferred `Show()` has to be completed from a sane task
context, and it is worth checking what else framebuffer-mode `Show()` does
besides the copy -- if it also re-points subsequent rendering at the
framebuffer, deferring it from a foreign task is not obviously safe and that
has to be understood before writing the code.

# What to do

1. Read AROS's framebuffer-mode Display class and establish exactly what
   `Show()` does beyond copying, and from which contexts it may be called.
2. Implement hold-and-release with the watchdog first, so the failure mode is a
   late desktop rather than no desktop.
3. Add the real signal on top, and keep the watchdog.
4. Confirm under QEMU by screendump: the splash should persist, and the desktop
   should appear with its icons already drawn.

# Notes

**Do not build this without the release path settled.** The whole risk is
concentrated in one place and it is a black screen.

# Execution log

- 2026-08-17 -- Opened after the splash-scaling work. Identified the takeover
  point and, more usefully, that this driver copies on Show, which is what makes
  the exception clean rather than a race.
