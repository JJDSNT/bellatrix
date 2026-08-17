---
id: ISSUE-0012
title: "Hardware mouse cursor on the VideoCore, so AROS stops compositing it in software"
status: backlog
priority: medium
type: feature
owner: unassigned
created_at: 2026-08-06
updated_at: 2026-08-06
tags:
  - graphics
  - videocore
  - emu68gfx
  - hardware
blockers:
  - "validation on real hardware (Pi 3B) — QEMU raspi3b emulates neither the firmware cursor tags nor the HVS"
  - "the HVS variant presupposes owning the display, which has not started"
related_files:
  - aros/arch/m68k-emu68/hidd/emu68gfx/emu68gfx_hiddclass.c
  - aros/arch/m68k-native/soc/mbox/mbox_init.c
  - external/aros/arch/arm-native/soc/broadcom/2708/hidd/vc4gfx/vc4gfx_hiddclass.c
  - external/aros/arch/arm-native/soc/broadcom/2708/include/hardware/videocore.h
  - external/aros/rom/hidds/gfx/gfx_cursorfbclass.c
---

# Summary

`emu68gfx` declares no hardware cursor, so AROS composites the pointer itself:
the graphics base class wraps the framebuffer bitmap in a `CursorFB` proxy that
saves and restores the pixels under the pointer on every move. Give the display
a real sprite and that whole layer disappears.

Two mechanisms exist on this silicon, and they are for different moments — the
firmware's mailbox cursor works today against the firmware framebuffer, and an
HVS plane is what this becomes once we drive the VideoCore ourselves.

# Problem

## What happens today

`emu68gfx_hiddclass.c:156-164` already documents the consequence, because it had
to work around it:

> Always name the class ourselves instead of letting the generic Display class
> fall back to "inherit the framebuffer bitmap's class" for displayable bitmaps:
> since we have no hardware cursor, that framebuffer bitmap is wrapped in a
> `CursorFB` proxy, and blindly instantiating THAT class via `OOP_NewObject()`
> (skipping its own `create_cursorfb()` constructor) leaves it with no real
> bitmap to forward to — every attribute `Get()` on it then hangs.

So the software cursor is not only a cost, it is already a trap this port has
had to steer around once. A hardware sprite removes the wrapper and the trap
with it.

The cost is worse here than on a native ARM target. Every pointer movement is a
read-modify-write of framebuffer pixels performed *by the m68k guest*, through
Emu68's JIT, against memory that is uncached MMIO-ish from the guest's point of
view. On `arch/arm-native` the same work is a native memcpy.

## The AROS-side contract

`rom/hidds/gfx/gfx.conf:99-101,458-460` — three methods, and an attribute that
tells the base class they are real:

```
BOOL SetCursorShape(OOP_Object *shape, WORD xoffset, WORD yoffset)
BOOL SetCursorPos(WORD x, WORD y)
VOID SetCursorVisible(BOOL visible)
```

The modern form lives on the **Display** class with
`aoHidd_Display_SpriteTypes` reporting `vHidd_SpriteType_DirectColor`, which is
the shape `emu68gfx` already has (`Emu68Display__Hidd_Display__CreateObject`).
Reporting a non-zero sprite type is what stops the base class installing
`CursorFB`.

## Mechanism 1 — the firmware's mailbox cursor

`VCTAG_SETCURSORINFO` (`0x8010`) and `VCTAG_SETCURSORSTATE` (`0x8011`). The
firmware composites a 64×64 ARGB image over whatever the display is showing;
nothing on the ARM side touches the scanout.

**A complete reference implementation is already in this tree**, for the same
silicon and the other CPU architecture:
`external/aros/arch/arm-native/soc/broadcom/2708/hidd/vc4gfx/vc4gfx_hiddclass.c`.
It is worth reading before anything is written, because it has already paid for
several surprises:

- the cursor buffer is GPU memory obtained with `VCTAG_ALLOCMEM` + `VCMEM_DIRECT`
  (the uncached `0xC` alias — `VCMEM_NORMAL` is rejected for ARM-side requests),
  and is held for the driver's lifetime because the firmware re-reads it on every
  `SETCURSORINFO`;
- the firmware **ignores** the hotspot it is given in `SETCURSORINFO` and places
  the image's top-left at the position, so AROS's hotspot has to be folded into
  the coordinates at `SetCursorPos()` time;
- `SETCURSORSTATE` takes a flag selecting framebuffer coordinates.

Everything it needs already exists on our side: `MBoxCall()` is in
`aros/arch/m68k-native/soc/mbox/`, and the little-endian conversion idiom the
whole port uses is the same one that file uses.

Its limits are the firmware's: 64×64 maximum, one sprite, and a mailbox round
trip on every pointer move — cheap compared with repainting the framebuffer, but
not free, and it is a guest→Emu68→firmware round trip here rather than a plain
ARM one.

## Mechanism 2 — an HVS plane

The Pi's display is composited by the HVS, which builds each output frame from a
display list of planes. A cursor is naturally one more plane: no size limit
imposed by a firmware ABI, no per-move mailbox call, and correct interaction
with whatever else we end up putting on screen.

It also means **owning the display**: today the firmware owns the HVS and hands
Emu68 a framebuffer through the mailbox (`init_display()`,
`external/emu68/src/raspi/start_rpi64.c:239`), and there is no HVS or pixel-valve
code anywhere in Emu68. Taking it over is the same piece of work as driving the
VideoCore directly, which is what this issue is filed against — the cursor is
one of the first things that becomes possible, not a reason to start.

# Goal

The pointer moves without any framebuffer pixel being rewritten by the guest,
and `CursorFB` is out of the picture.

# What is left

Everything. The order that keeps each step testable:

1. Report `aoHidd_Display_SpriteTypes` and implement the three methods against
   **mechanism 1**, following the `vc4gfx` reference. This is the smaller piece
   and it is independent of the VideoCore work.
2. Confirm the base class stops wrapping the framebuffer — the wrapper is
   observable, and the workaround comment in `CreateObject` should be revisited
   rather than left describing a state that no longer holds.
3. When the display is ours, move the implementation to an HVS plane behind the
   same three methods. The AROS-facing contract does not change, which is the
   point of doing step 1 first.

# Decisions taken

None yet. Recorded so the framing is not lost: mechanism 1 is a bridge, not the
answer. The answer is the plane, and it arrives with the VideoCore work.

# Acceptance criteria

- [ ] `aoHidd_Display_SpriteTypes` reports a non-zero sprite type
- [ ] The framebuffer bitmap is no longer a `CursorFB` proxy
- [ ] The pointer moves over the Workbench backdrop leaving no trail and
      repainting nothing
- [ ] Shape changes (busy pointer) and hide/show both work
- [ ] Validated on a real Pi 3B — see the blockers
- [ ] The workaround comment in `Emu68Display__Hidd_Display__CreateObject` either
      still applies or is corrected

# Notes

**This cannot be validated under QEMU, and that is now checked rather than
assumed.** `raspi3b` provides a mailbox-driven simple framebuffer and no HVS.
The binary (`qemu-system-aarch64` 8.2.2) carries
`bcm2835_property: unhandled tag 0x%08x` and no BCM2835 cursor handling at all,
so `0x8010`/`0x8011` land in the unhandled-tag branch.

That is exactly the dangerous shape: the property interface reports overall
success on a message whose individual tag was ignored, so a driver written
against it looks like it works and shows nothing. **The cheap positive test is
`-d guest_errors`** — the unhandled tag is logged by name, which turns "nothing
appeared" into "the tag never ran". Worth wiring into whatever run is used
before concluding anything about the driver.

The `vc4gfx` reference happens to be defensive here: it checks that the
firmware wrote a result back into the request buffer rather than trusting the
message-level status. Keeping that check is the difference between a silent
failure and a diagnosable one.

The `nocomposition` boot argument is currently required to see anything on the
framebuffer at all. Whatever is done here has to hold under that, and a cursor
that only works with compositing enabled is not yet usable.

# Execution log

- 2026-08-06 — opened. Found while writing it that the mailbox cursor is already
  implemented in this tree for `arch/arm-native`, which turns step 1 from a
  driver to be invented into a port of a working one.
- 2026-08-06 — checked the QEMU claim against the binary instead of leaving it
  as an expectation: no BCM2835 cursor handling, and an unhandled-tag log path
  that `-d guest_errors` makes visible. Recorded as the positive test.

# Position in the queue

Deliberately not next. The port's open problem is that 6 of 10 boots do not
reach the desktop at all (ISSUE-0007), and nothing here moves that. This is also
the only open issue that cannot be advanced under QEMU — every step needs a Pi,
so it is naturally the work to batch with other hardware-only questions rather
than to interleave with what can be measured locally.
