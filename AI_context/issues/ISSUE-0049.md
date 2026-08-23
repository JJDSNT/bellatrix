---
id: ISSUE-0049
title: "The boot presentation starts its clock late and ends before the icons"
status: open
priority: medium
type: bug
owner: unassigned
created_at: 2026-08-23
updated_at: 2026-08-23
tags:
  - bootui
  - graphics
  - boot
blockers:
related_files:
  - aros/arch/m68k-emu68/boot/bootui.c
  - aros/arch/m68k-emu68/boot/bootui_platform.c
  - aros/arch/m68k-emu68/platform/bcm283x/system_timer.c
  - aros/arch/m68k-emu68/include/aros/bootui.h
  - patches/aros/0033-wanderer-tell-the-boot-splash-when-the-desktop-has-icons.patch
  - external/emu68/src/aarch64/start.c
---

# Summary

Two defects in the boot presentation, both about *when* rather than *what*.
Neither affects correctness of the display; both make the presentation say
something untrue about the boot.

## 1. The clock does not start when the machine starts

`bootui_clock_start()` has exactly one caller:

```
aros/arch/m68k-emu68/platform/bcm283x/system_timer.c:150
    bootui_clock_start(systimer_read(SYSTIMER_CLO));
```

and it sits at the end of `systimer_init()` -- after the platform node has
been matched, after `KrnAddIRQHandler()` has succeeded. So the elapsed time on
screen is measured from *the AROS system timer coming up*, which is neither
when the machine was switched on nor when this port started running.

The moment worth measuring from is the one Emu68 already prints, at
`external/emu68/src/aarch64/start.c:2331`:

```
[JIT] Let it go...
```

That is where the JIT stops preparing and starts executing m68k code -- the
first instant at which anything in this issue's timeline is our code's fault.
Everything before it is Emu68 bringing up the hardware, which the presentation
is not reporting on and cannot influence.

Concretely, from a QEMU boot: the ELF is loaded and the JIT released within
the first serial lines, while the first timestamped BootUI line is

```
[BootUI] [00:02.982] retargeted to RGB32 framebuffer
```

Those ~3 seconds are real boot time that the clock does not count, and the
gap is not a constant: it contains the platform scan, so it moves with what
hardware is found.

The fix is not "call `bootui_clock_start()` earlier" -- there is no AROS timer
to read before the platform is up. It needs the origin to come *from Emu68*,
handed across the boot context the way the arguments and the framebuffer
already are, so that AROS computes elapsed time against a stamp it did not
take. `SYSTIMER_CLO` is free-running and is the same counter on both sides of
the handover, so an Emu68-side reading of it at "Let it go" is directly
comparable with the AROS-side readings.

## 2. The hold ends at the driver handover, not at the icons

The design, in `bootui.c`'s own comment (lines 62-102), is that the hold ends
when Wanderer's backdrop window has its volume icons -- signalled by
`patches/aros/0033`, which posts `BOOTUI_STAGE_ICONS` through
`bootui.resource` from Wanderer's own task, so the icons are in the bitmap
before the splash gets out of the way. That is the behaviour the user asked
for ("o certo é ele sair nos ícones").

That is not what happens. Every boot measured on 2026-08-23 ends the
presentation on the scanout handover instead:

```
[BootUI] STARTING WANDERER...
[BootUI] [00:26.585] display takeover: direct scanout
```

`display takeover: direct scanout` is the last BootUI event of the boot, in
every run, with or without mungwall. `BOOTUI_STAGE_ICONS` never arrives, so
the two mechanisms that should end the hold on the icons -- the arm, and the
quiet-period release that follows it -- never run. What ends the presentation
is the display driver taking scanout, which happens when the driver is ready
and says nothing about whether the desktop is drawn.

The visible consequence is the one the user reports: the splash leaves before
the desktop has icons, so several seconds of half-built desktop are shown --
exactly what the hold exists to prevent.

Note the history: an earlier revision of this code did end on the icons, and
did it with a heuristic rather than with the `BOOTUI_STAGE_ICONS` signal. The
signal path is the better design and should be the one repaired; the heuristic
is worth reading only for what it observed, not for reinstating.

# What to check first

- ~~Whether `patches/aros/0033` is still reaching the built module.~~ Checked
  on 2026-08-23: it does. The call is not in `System/Wanderer/Wanderer` --
  `iconvolumelist.c` builds into a separate Zune class,
  `Classes/Zune/IconVolumeList.mui` -- and both that file and `sd.img` contain
  `bootui.resource`. So the signal is on the card and the search moves past
  the build.
- Whether `bootui.resource` is registered by the time Wanderer looks for it.
  `bootui_add_resource()` has to have run, and `patches/aros/0033` gives up
  permanently on the first miss (`bootui_told = TRUE` is set whether or not
  the resource was found).
- Whether the scanout handover should end the presentation at all. The
  handover means "a native driver owns the display", which `bootui.c` already
  handles as `BOOTUI_STAGE_HANDOVER` without releasing. If a second path is
  also calling `bootui_takeover()` on the same event, that is the bug.

# Not related to ISSUE-0037

Measured on 2026-08-23: with `nobootui` on the kernel command line -- a switch
added the same day, which makes `bootui_init()` decline the surface and draw
nothing for the whole boot -- the TLSF free-list corruption still occurs (2 of
4 runs). The boot presentation is not the memory writer.

It does change the *timing*, and sharply: with the presentation running the
corruption is deterministic (8 of 8), with it off it returns to intermittent.
That makes it a useful lever for reproducing ISSUE-0037, and a variable that
has to be held fixed when measuring it.
