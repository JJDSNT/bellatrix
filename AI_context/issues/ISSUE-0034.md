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

# The release signal already exists

There is no need to invent one. `C:BootProgress` (`arch/m68k-emu68/c/BootProgress.c`)
opens `bootui.resource` and calls `set_stage`, and patch 0015 has
`S:Startup-Sequence` calling it at three points. It already maps `WANDERER` to
`BOOTUI_STAGE_DESKTOP`.

**The signal that releases the hold is the same one that says the icons are
there.** That is one signal, not a choice between several -- and the only thing
wrong with the one we have is where it is sent from:

```
If EXISTS "C:BootProgress"
    C:BootProgress WANDERER      <- fires here
EndIf

WANDERER:Wanderer                <- icons start being drawn only now
```

It is published *before* Wanderer is launched, so today `BOOTUI_STAGE_DESKTOP`
means "about to start the desktop", not "the desktop is drawn". Held on that,
the release would arrive before the screen it is meant to wait for.

**`SYS:WBStartup/` is where it should be sent from.** Workbench launches those
items after it has opened its window and laid the icons out, which is the
meaning wanted, and it needs no upstream change -- only a small program on the
card, because a Workbench-launched tool gets a startup message rather than the
command line `BootProgress` parses.

**Keep a watchdog underneath it regardless.** Not as an alternative signal but
as the floor: if the card is missing that item, or it fails, the desktop must
still appear. A hold that is never released is a machine that boots to a splash
forever, and that is the whole risk of this change.

# What to do

1. Read AROS's framebuffer-mode Display class and establish exactly what
   `Show()` does beyond copying, and from which contexts it may be called. This
   is the one open technical question and it decides whether the deferred Show
   can be completed from another task at all.
2. Implement hold-and-release with the watchdog alone first, so the worst
   failure while developing is a late desktop rather than none.
3. Add the WBStartup item that publishes the stage, and keep the watchdog.
4. Confirm under QEMU by screendump: the splash should persist, and the desktop
   should appear with its icons already drawn.

# The verification already exists too, and it is not the signal

`scripts/boot-timing.py` already decides when the icons are there: it samples
the framebuffer and declares them when the changed-pixel count grows past
`--icon-delta` beyond the delta the bare screen produces, corroborated by the
screen title changing from "Workbench Screen" to "Wanderer <n>M graphics mem".

**That is a host-side observation, so it cannot release the hold** -- it reads
the screen from outside the machine, and the thing being held is inside it.
What it is good for is the other half: it is the oracle that proves this change
did what it claims. Its `t1` is defined as the icons rather than the screen,
which is exactly the interval this issue is about, so a before/after run of it
measures the gap being closed rather than anyone judging a screenshot.

# Notes

**Do not build this without the release path settled.** The whole risk is
concentrated in one place and it is a black screen.

**Frozen as of 2026-08-17.** This is an addition, and nothing new is added
until the system is fast and stable (see `CLAUDE.md`). The design above is
finished and stays here as the record; it is not a task waiting to be picked up.

# Execution log

- 2026-08-17 -- Opened after the splash-scaling work. Identified the takeover
  point and, more usefully, that this driver copies on Show, which is what makes
  the exception clean rather than a race.
