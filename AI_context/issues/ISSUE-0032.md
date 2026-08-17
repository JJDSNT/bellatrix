---
id: ISSUE-0032
title: "BootUI is drawn for a 640x480 screen: no image on a real Pi, and a loader three times too small"
status: in-review
priority: medium
type: bug
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - bootui
  - graphics
  - emu68
  - legacy
blockers:
related_files:
  - aros/arch/m68k-emu68/boot/bootui.c
  - aros/arch/m68k-emu68/boot/bootimage.inc
  - scripts/make-boot-image.py
---

# Summary

Every measurement in the boot UI is an absolute pixel count chosen for a
640x480 screen. A real Pi 3 comes up at 1920x1080, so the splash image is not
drawn at all and everything that is drawn is a third of the size it should be,
pinned to the bottom edge of the screen.

Legacy solved this and the solution is four lines
(`~/bellatrix-legacy/src/launcher/launcher_ui.c:30`):

```c
lui_scale = fb_width / 640u;
if (lui_scale < 1u) lui_scale = 1u;
if (lui_scale > 3u) lui_scale = 3u;
lui_char = 8u * lui_scale;
```

# The image does not appear, and says nothing about it

`bootui.c:95`

```c
if (bootui.width != BOOT_IMAGE_WIDTH || bootui.height != BOOT_IMAGE_HEIGHT)
    return 0;
if (bootui.pitch != BOOT_IMAGE_WIDTH * 2)
    return 0;
```

`bootimage.inc` is generated at 640x480. On any other mode the function returns
0 and nothing is drawn -- silently, with no message on the serial console, so
the symptom is a blank screen with a loader on it and no clue why.

The guard is not paranoia: the drawing loop below it walks the run-length data
into `framebuffer` as one flat sequence of pixels, with no per-row addressing
at all, so it is only correct when the framebuffer's pitch happens to equal the
image's width. Making the image appear at 1920x1080 therefore means more than
relaxing the test -- it needs row-by-row addressing and a decision about what to
do with the leftover screen.

Three ways out, in increasing order of work:

1. **Centre it, unscaled.** Row addressing plus an origin offset. Cheapest,
   and a 640x480 logo in the middle of a 1080p screen looks like a postage
   stamp.
2. **Integer-scale it**, by the same factor the rest of the UI should use. Run-
   length data scales cheaply -- each run just emits N times as many pixels per
   row, and each row is emitted N times.
3. **Generate the image at the real mode.** Rejected on sight: the mode is not
   known at build time.

Option 2 is the one that matches what legacy did and what the loader needs
anyway.

# The loader is sized for a screen a third as wide

`bootui.c:215-226`, with what each becomes at 1920x1080:

| | code | at 640x480 | at 1920x1080 |
|---|---|---|---|
| bar width | `bootui.width / 2` | 320 | 960 -- **scales, fine** |
| bar height | `6` | 6 | 6 -- a hairline |
| text scale | `draw_text(..., 1, ...)` | 5x7 glyphs | 5x7 glyphs -- unreadable |
| band height | `bootui.height - 90` | 90 | 90 |
| bar position | `bootui.height - 26` | 26 from bottom | 26 from bottom |

Only the width scales. Everything vertical is a constant, which is why the
whole thing reads as correct at 640x480 and as a thin line stuck to the bottom
edge of a 1080p display.

# And it sits too low

`bar_y = bootui.height - 26` puts the bar 26 pixels from the bottom of the
screen whatever the screen is. On a 480-line display that is a reasonable
margin; on a 1080-line one it is against the edge. It should be lifted -- a
proportion of the height rather than a constant, so it sits above the bottom
rather than on it.

# Done, and what is left to confirm

All four changes are in `bootui.c`. Verified under QEMU at 1920x1080: the
splash appears, centred and doubled, with a thicker loader lifted clear of the
bottom edge and readable status text. **Not yet confirmed on a real Pi**, which
is where the symptom was reported.

One correction to the plan below: the factor cannot come from the width alone.
1920/640 is 3, but 1080/480 is 2, and scaling the splash by 3 would ask for
1440 lines on a 1080-line display. It is the smaller of the two, clamped to
1..3, which is why a Pi gets 2.

The bar's width was already right and is unchanged -- `width / 2`. What changed
is its height (`10 * scale`, was a flat 6) and the margin under it
(`52 * scale`, was a flat 26), at every resolution rather than only on large
ones, because the bar was thin and low on a 640x480 screen too.

# The original plan

1. **Derive one scale factor from the framebuffer width**, as legacy did:
   `width / 640`, clamped to 1..3. Every constant below then multiplies by it.
2. **Multiply the vertical constants by it**: bar height, band height, the gap
   between the text and the bar, and the text scale passed to `draw_text`.
3. **Lift the bar** by making its distance from the bottom proportional rather
   than fixed.
4. **Scale and centre the splash image** with per-row addressing, using the
   same factor.
5. **Say when the image is skipped.** Whatever the outcome, a `return 0` that
   produces a blank screen should print the mode it refused, so the next person
   does not have to read this file to find out.

# Notes

**This is the first thing anyone sees**, and it is currently at its worst on
the only hardware that matters. It reads as a half-finished boot rather than as
a resolution mismatch.

**QEMU hides it**, because the framebuffer there has been coming up at the same
1920x1080 as the Pi -- so this is not a hardware-only fault, it is simply that
nobody has looked at the splash rather than at the serial log.

# Execution log

- 2026-08-17 -- Fixed and verified under QEMU at 1920x1080. Awaiting a look on
  real hardware.
- 2026-08-17 -- Opened from an observation on a real Pi 3: no image, and a
  loader too small to read. Traced to the 640x480 constants and matched against
  the scaling legacy already had.
