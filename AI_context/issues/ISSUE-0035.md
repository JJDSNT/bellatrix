---
id: ISSUE-0035
title: "Icons on the Wanderer desktop cannot be moved"
status: backlog
priority: medium
type: bug
owner: unassigned
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

# Summary

Icons on the desktop do not move. Pressing one and dragging leaves it where it
was.

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

# What to do

1. Boot and answer the four questions in "What this claims". Write down which,
   not a summary.
2. Repeat under QEMU. If it reproduces there, everything after this is cheap;
   if it does not, that is itself a strong result and points at input.
3. Only then pick a candidate.

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

- 2026-08-17 -- Opened on the user's report, alongside the decision to stop
  adding features until the system is fast and stable. No diagnosis yet: the
  drag path was read to name the candidates, nothing was measured.
