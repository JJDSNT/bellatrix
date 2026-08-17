---
id: ISSUE-0035
title: "Icons on the Wanderer desktop cannot be moved"
status: done
priority: medium
type: bug
owner: agent
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - wanderer
  - zune
  - input
  - graphics
  - emu68gfx
blockers:
related_files:
  - external/aros/workbench/system/Wanderer/Classes/iconlist.c
  - external/aros/workbench/libs/muimaster/classes/area.c
  - external/aros/workbench/libs/muimaster/classes/window.c
  - external/aros/workbench/libs/muimaster/dragndrop.c
  - aros/arch/m68k-emu68/hidd/emu68gfx/emu68gfx_hiddclass.c
---

# Resolved on main (2026-08-17), with the cause unidentified

Icons move. Confirmed on the Pi by the user, and confirmed harder than intended
-- the test accidentally dropped the `C` drawer into another drawer, so
`MUIM_DragQuery`, `MUIM_DragDrop` and Workbench actually moving a directory all
work, not merely the icon sliding.

**What fixed it is known; why it broke is not.** `arch/m68k-emu68/` was put
back to what `v0.1.0-rc1` shipped, for an unrelated reason -- that the modular
kickstart work is not the path being taken -- and the symptom went with it. The
cause is therefore in the `kickstart-base-package` branch or the
`m68k-native-split` branch and nowhere else.

Closed rather than pursued because finding out which requires merging those
branches back one at a time, and the standing freeze says they stay parked. The
two-merge plan below is kept intact: if either branch is ever revived, it is
the first thing to run, and it has to watch for a *drop landing* rather than an
icon sliding.

The four candidate mechanisms below were never tested and are kept for the same
reason -- if this returns, they are still the list.

# Summary

Icons on the desktop do not move. Pressing one and dragging leaves it where it
was.

# Confirmed: it was one of the two parked branches

**2026-08-17, on hardware: the icons move again.** `arch/m68k-emu68/` is back
to what `v0.1.0-rc1` shipped, so the defect is in the `m68k-native-split`
branch or the `kickstart-base-package` branch and nowhere else. The four
candidates below are no longer four independent possibilities -- they are ways
that a *known* change could have produced the symptom.

That turns the remaining work into two merges, cheapest first:

1. **Merge `kickstart-base-package` and try again.** Two patches and a document.
   The only thing in it that runs on this target is
   `arch_CauseBlitterInterrupt()` becoming empty where `qblit.c`/`qbsblit.c`
   used to store to `$DFF000` under a `defined(mc68000)` gate. On a machine with
   no chipset both should be no-ops, which makes this the one to eliminate
   first, not the one to suspect.
2. **If the icons still move, it is `m68k-native-split`**, and the search
   narrows to its three move commits plus the three behavioural changes that
   travelled with them: the memory-range gate in `boot.c`, the
   `m68k_boot_putc` sink, and the `PlatformClockObserver` indirection.

Do them one at a time. Merging both and testing once answers nothing.

# What this claims, and what it does not

**Claimed:** on real hardware, an icon that is pressed and dragged stays put.

**Not claimed, and not yet known:**

- whether the drag *starts* at all (does the icon become selected? does a drag
  image appear and follow the pointer?),
- whether it fails for every icon or only for the ones this project generates,
- whether anything moves and then snaps back,
- whether the same happens under QEMU.

Those four questions are the whole investigation. Each of them is one boot, and
none of them needs a code change first.

# The path a drag takes, so the candidates are nameable

```
IconList press          Wanderer/Classes/iconlist.c:6338   DoMethod(obj, MUIM_DoDrag, ...)
  -> Area MUIM_DoDrag   muimaster/classes/area.c:2166      -> MUIM_Window_DragObject
  -> HandleDragging()   muimaster/classes/window.c:1502    the drag loop, fed by IDCMP_MOUSEMOVE
  -> drag image         muimaster/dragndrop.c              own Layer_Info/Layer (:121),
                                                           saves and restores what is under it
                                                           with BltBitMap/BltBitMapRastPort
  -> drop               MUIM_DragQuery / MUIM_DragDrop back into the IconList
```

Every one of those stages can fail silently and produce exactly the same
symptom, which is why the symptom alone decides nothing.

# Candidates, cheapest discriminator first

1. **Only our icons.** The BTScan icon was unopenable for one reason: the
   `Gadget.Activation` field was zero instead of `GACT_RELVERIFY` (see
   `external/aros-bluzing/images/mkicon.py`). A hand-written icon file is
   exactly the kind of thing that is wrong in one field and looks right. If the
   *system* icons move and ours do not, this is the icon writer and nothing
   else. **Try a system icon first** -- one boot, no build.
   (`do_CurrentX/Y` are `NO_ICON_POSITION`, which is correct for a new icon and
   is not this.)

2. **`IDCMP_MOUSEMOVE` while the button is held.** `HandleDragging()` is driven
   by mouse-move messages. Clicks demonstrably reach Intuition on this port, but
   that only proves button events arrive -- it says nothing about motion being
   reported to the window during a press. If motion is coalesced, delivered only
   on release, or not requested, the loop runs once and the icon never leaves.

3. **The drag image cannot be drawn.** `dragndrop.c` creates its own layer and
   saves/restores the pixels beneath the drag image. This port has **no
   hardware cursor** -- the framebuffer bitmap is wrapped in a CursorFB proxy
   (`emu68gfx_hiddclass.c:158-164`) and the pointer is composited in software.
   Two producers saving and restoring overlapping rectangles of the same linear
   framebuffer is a plausible way for a drag to be invisible, or visible and
   immediately erased. Note this candidate predicts *movement that does not
   show*, not an icon that is inert.

4. **The drop is refused.** The icon moves under the pointer and returns
   because the IconList rejects the drop on itself. This one is distinguishable
   by eye: something visibly happens.

# QEMU cannot answer this, and that is worth knowing before trying

There is **no pointing device under QEMU**. `run.sh` attaches `-device
usb-tablet` only for graphical runs, and attaching it by hand does not help:
the controller sees it -- `[USB2OTG] Init: Device connected, resetting port` --
and nothing enumerates it. The serial log has no HID line, `mouse_move` through
the QEMU monitor changes not one pixel, and the pointer sits where it started.

So the oracle for this issue is hardware, and every candidate below has to be
answered there. `boot-timing.py` and `screendump` still work for everything
*except* the pointer.

# What to do

1. On the Pi, answer the four questions in "What this claims". Write down which,
   not a summary.
2. Only then pick a candidate.

# Notes

**This issue is under the standing freeze**: nothing new is added to the system
until it is fast and stable. Diagnosing this is not an addition -- a desktop
whose icons cannot be moved is not a stable desktop -- but resist the urge to
fix it by *adding* anything. The likely outcomes here are a corrected field in
an icon file, an event that is not being requested, or a drawing conflict that
already exists.

**Do not assume this is one bug.** "Icons do not move" is a symptom shared by
four independent mechanisms, and this port has recent history of two defects
being read as one.

# Execution log

- 2026-08-17 -- **Confirmed on the Pi: the icons move.** The revert below was
  done for a different reason and this was not what it was for, so treat it as
  what it is -- a bisect step that landed, narrowing the cause to two branches.
  Confirmed harder than intended: the test accidentally dropped `C` into
  another drawer, which means the whole path works, not just repositioning --
  `MUIM_DragQuery`, `MUIM_DragDrop` and Workbench actually moving the
  directory. So the two merges below have to watch for a drop landing, not
  merely an icon sliding. (The card was regenerated afterwards; a drawer that
  moved is a real move.)
- 2026-08-17 -- `arch/m68k-emu68/` put back to what `v0.1.0-rc1` shipped, which
  is the last state in which the drag is known to have worked; the m68k-native
  split that followed it is on the `m68k-native-split` branch and the BASE
  package work on `kickstart-base-package`. This is not a fix and is not
  evidence: it was done because that work is not the path being taken, and the
  side effect is that the suspected window is now empty. If the drag works on
  the next hardware boot, the cause is in one of those two branches and merging
  them back one at a time says which. If it does not, the regression predates
  the release and the four candidates all still stand.
- 2026-08-17 -- Tried to reproduce under QEMU and could not: no pointing device
  enumerates there. Recorded above, because it decides where this can be worked
  on at all.
- 2026-08-17 -- Opened on the user's report, alongside the decision to stop
  adding features until the system is fast and stable. No diagnosis yet: the
  drag path was read to name the candidates, nothing was measured.
