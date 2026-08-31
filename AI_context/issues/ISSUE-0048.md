---
id: ISSUE-0048
title: "The software compositor swallows the display, and nothing else needs nocomposition"
status: open
priority: medium
type: bug
owner: unassigned
created_at: 2026-08-22
updated_at: 2026-08-22
tags:
  - graphics
  - compositor
  - raspberry-pi-3
blockers:
related_files:
  - external/aros/rom/graphics/graphics_display.c
  - external/aros/workbench/devs/monitors/Compositor
  - aros/arch/m68k-emu68/hidd/vcgfx
  - aros/arch/m68k-emu68/hidd/fbgfx
  - scripts/make-sdcard.sh
---

# Summary

Every Bellatrix card boots with `nocomposition` on the kernel command line,
and without it nothing reaches the framebuffer. No other AROS target does
this: `arch/aarch64-raspi` writes no command line at all and boots with the
software compositor installed. If aarch64 does not need the flag on the same
hardware, neither should we, and the flag is covering a defect rather than
expressing a preference.

# Measured, not assumed

Two runs of the same build, screendumped at the same point (2026-08-22):

| boot argument   | dominant colour                                            |
|-----------------|------------------------------------------------------------|
| `nocomposition` | 92.0% `999999` Workbench grey, 4.3% white, 1.9% title blue |
| none            | 89.6% black, the rest boot-splash leftovers                |

This was re-measured deliberately after the display drivers changed. The
README used to attribute it to `emu68gfx` missing something the compositor
wants; that guess is dead. The run above had `fbgfx` as the boot driver and
`vcgfx` installed from `DEVS:Monitors` -- neither of them emu68gfx -- and the
behaviour is unchanged. Whatever the compositor wants is missing from all
three of our drivers.

# Where it is decided

`display_Setup()` (`rom/graphics/graphics_display.c:120-190`) sets
`DF_SoftComposit` when a driver offers a truecolor mode and does not declare
hardware composition through `HIDD_Display_ModeProperties`'s
`CompositionFlags`. Ours are truecolor and declare nothing, so the software
compositor is installed and `display_LoadViewPorts()` routes presentation
through `compositor_LoadViewPorts()` instead of a plain `Show()`.

What happens inside that path on this port is not known. The boot runs to
completion either way -- this is not a hang -- so the compositor is
succeeding at something and the result is not being scanned out.

# Next evidence

1. Boot without the flag and log inside `compositor_LoadViewPorts()`: does it
   composite, and what does it Show?
2. Compare with aarch64-raspi on the same hardware if a card can be made --
   it is the control that says whether this is ours or the compositor's.
3. Check whether the compositor's own bitmap is created with a DisplayID our
   drivers accept; `display_Register()` warns that the id base has to be
   assigned before the compositor is set up, which is a sequencing rule
   somebody has already had to fix once.
4. `HIDD_Display_ModeProperties` on our drivers: what CompositionFlags do
   they report, and is reporting none of them honest?

# Acceptance criteria

- A card without `nocomposition` reaches the same desktop as one with it.
- The default command line stops carrying the flag.
- Why it was needed is written down, not just that it no longer is.

# Notes

`nomonitors` is a different flag and must not be used as a substitute: it sets
`BF_NO_DISPLAY_DRIVERS`, which stops `AROSMonDrvs` running `DEVS:Monitors` --
now where the VideoCore driver lives. A boot with it silently keeps the
kickstart boot driver and never takes the hardware.

`scripts/make-sdcard.sh` takes `BELLATRIX_CMDLINE` to override the whole line,
which is how the measurement above was taken. A card that cannot be built
without the flag cannot test the flag.

# Execution log

- 2026-08-22 -- Opened after the flag was re-measured against a completely
  different pair of display drivers and turned out to be as necessary as
  before, which killed the standing explanation for it.
