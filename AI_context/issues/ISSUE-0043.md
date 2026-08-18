---
id: ISSUE-0043
title: "Drive the VideoCore display ourselves, instead of borrowing a framebuffer from the firmware"
status: backlog
priority: medium
type: feature
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - videocore
  - graphics
  - emu68
  - performance
  - bootui
blockers:
related_files:
  - aros/arch/m68k-emu68/hidd/emu68gfx/emu68gfx_hiddclass.c
  - aros/arch/m68k-emu68/boot/bootui.c
  - aros/arch/m68k-emu68/boot/boot.c
  - external/aros/arch/arm-native/soc/broadcom/2708/include/hardware/videocore.h
  - AI_context/issues/ISSUE-0012.md
---

# Authorised as an exception to the freeze

The standing decision of 2026-08-17 says nothing new goes in until the system
is fast and stable, and that an open issue is not permission to build the thing
it describes. **This one is permission**, given by the user on 2026-08-17, with
their reasons recorded so the exception is legible later:

* it is work this project will do sooner or later,
* the boot UI gave it a second reason (below), and
* there is a plausible performance gain (below, stated more narrowly than
  "should be faster").

The trade was made with ISSUE-0037 open but quiet. The user's terms: *"não tive
mais nenhuma ocorrência de erro, se ele retornar retornamos a ele."* The guard
bytes in `patches/aros/0030` and `S:ram-stress-c` stay on the card, so if the
heap corruption reappears during this work it identifies itself rather than
becoming noise inside a new driver.

# The fork this issue exists to decide

"Porting the VideoCore" names two quite different projects, and picking between
them is the first task, not an implementation detail.

## Path A — stay a client of the firmware, over the mailbox

Ask the VideoCore firmware for a framebuffer through the property interface:
`SET_PHYSICAL_WIDTH_HEIGHT`, `SET_VIRTUAL_WIDTH_HEIGHT` with twice the physical
height, then flip by writing `SET_VIRTUAL_OFFSET` and pacing on
`FB_WAIT_FOR_VSYNC`.

* **Cheap.** Well-trodden, documented in the firmware's property interface, and
  this port already has `mbox.resource` — though **no framebuffer tags are
  implemented in it today**, which is the one concrete gap already measured.
* **But every flip is a mailbox round trip.** On this port that is a bus
  transaction from JITted m68k code, and the cost of those is exactly what this
  project keeps discovering is not free. A per-frame mailbox call is a
  different proposition from a per-boot one.
* And it leaves the firmware owning the display, so modes, timing and planes
  are whatever the property interface exposes.

## Path B — drive the display hardware directly

Program the VideoCore's own display pipeline: the HVS composing planes from
display lists, the PixelValve producing timing, and the encoder behind it.
A page flip becomes rewriting a display-list pointer — **no firmware, no
mailbox, no copy.**

* This is what "porting the VideoCore" properly means, and it is what ISSUE-0012
  is already filed against: a hardware cursor is *an HVS plane*, and that issue
  says so explicitly — *"the HVS plane is what this becomes once we drive the
  VideoCore ourselves."* So the two issues share a foundation and B is what
  unblocks it.
* **Much more work, and the documentation is not on our side.** The public
  BCM2835 ARM peripherals datasheet does not cover HVS, PixelValve or HDMI. The
  practical references are Linux's `vc4` driver and the reverse-engineering work
  behind it.
* **Ownership is the real risk, and it is not established.** On a Pi the
  VideoCore boots the machine and the firmware sets the display up before
  anything of ours runs. Taking the pipeline over while the firmware still
  believes it owns it — clocks, power, HDMI state — is the part most likely to
  produce a black screen with no diagnosis. How to make the firmware let go,
  or never take it, has to be answered before any register is written.

**Nothing here decides between them yet.** Path A is a smaller thing that may
be enough; Path B is the thing that makes ISSUE-0012 possible and removes the
firmware from the frame loop. The decision should be made on measurement, and
the measurement is named below.

# Why this earns its place: the copy, stated precisely

The user's expectation is that this brings performance. It can, but not for the
reason double buffering is usually wanted — **double buffering removes tearing,
not work.** The work is somewhere else and it is worth being exact about it,
because it is the number that justifies the project.

This port has one linear framebuffer, handed over ready-made by Emu68
(`emu68_bootstrap()`, stored at `boot.c:650-659`; nothing of ours ever asks the
VideoCore for it). AROS's generic Display class then **copies** the shown
bitmap into that framebuffer:

> *"copies the HIDD object to the framebuffer whenever a screen is shown"*
> — `emu68gfx_hiddclass.c:133-137`

If the scanout can be pointed at a buffer instead, that copy disappears. Under
Path A it becomes a virtual-offset write; under Path B a display-list pointer.
Either way the blit stops happening.

**Measure it before building anything.** How much time goes into that copy, per
`Show()` and per second of desktop use, on hardware? Until that number exists,
"this should give us performance" is a hope; with it, the port has a target and
a way to tell whether it hit it. Per the project's measurement discipline the
number that decides is on the Pi, not under QEMU, where MMIO and memory costs
are distorted.

# The second reason: the boot UI hand-off

`emu68_bootui_takeover()` is called from `Emu68Display__Hidd_Display__Show()`
the instant Intuition installs its first bitmap, because there is one
framebuffer and two producers would race for it. The consequence is that the
splash disappears the moment Wanderer's screen opens and well before it has
drawn any icons.

The user asked for the splash to survive until the icons are up. On the current
single-buffer arrangement that means repainting over Wanderer continuously —
flicker, and CPU burned during boot, which is the opposite of the point. With a
flip, the hand-off becomes a single instant: the splash owns the visible buffer,
Intuition composes into the other, and one flip swaps them when we choose.

**This is a beneficiary, not a justification.** It is cosmetic, it is the least
urgent item on any list here, and it should not be what decides between Path A
and Path B.

# Path C, and it is the one the user named: port `aarch64-raspi/hidd/fbgfx`

AROS already has a Raspberry Pi framebuffer HIDD for aarch64, at
`arch/aarch64-raspi/hidd/fbgfx/`. Porting it is the same mechanism this project
already used for the SDHOST backend, `dma.resource` and `usb2otg`, and it
should be assumed to be the cheapest route until shown otherwise.

**What it gives us, checked by reading the directory rather than assumed:**

```
fbgfx.conf  fbgfx_init.c  fbgfx_hiddclass.c
fbgfx_bitmapclass.c  fbgfx_displayclass.c  fbgfx_support.c
```

That is a *complete* HIDD. Ours is `emu68gfx_hiddclass.c` plus
`emu68gfx_init.c` and leans on AROS's generic bitmap and display classes for
everything else -- which is exactly why the generic Display class ends up
copying the shown bitmap into our framebuffer. A driver with its own
bitmapclass and displayclass is where that decision stops being someone
else's.

**What it does not give us.** Grepped for `SET_VIRTUAL_OFFSET`,
`SET_VIRTUAL`, `VCTAG`, `mbox` and `Mailbox` across the whole directory:
**no matches.** It is a framebuffer HIDD, not a VideoCore driver -- it manages
bitmaps over a framebuffer it is handed, the same one we are handed. So on its
own it delivers neither the flip nor the removal of the copy, which are the two
things this issue was opened for.

That makes Path C a **foundation rather than an alternative**: it is plausibly
the right first move under either A or B, because both need a driver that owns
its bitmaps before either can own the scanout. Worth establishing before
committing to it: where `fbgfx` gets its framebuffer, and whether its
displayclass still leaves the generic copy in place.

# What is not known

Ordered by how much each would change the plan:

1. **Will Emu68 give us a framebuffer with twice the virtual height**, or does
   the request have to be re-made from AROS after boot? The one we have arrives
   already allocated and we never asked for it.
2. **Does `emu68gfx` survive the scanout address changing underneath it?** It is
   built around exactly one always-mapped linear framebuffer
   (`emu68gfx_hiddclass.c:76,133-180`) and hands that address to the generic
   Display class as *the* framebuffer bitmap. Flipping means that address is no
   longer constant.
3. **How does the firmware release the display** — needed only for Path B, and
   the largest single unknown in it.
4. **What does the copy actually cost?** See above. This is the cheapest of the
   four and the one that decides whether any of it is worth doing for speed
   rather than for ISSUE-0012 and tidiness.

# Notes

**Do not merge this with ISSUE-0012.** That one is the hardware cursor, which
becomes possible under Path B and stays impossible under Path A. It is a
consumer of this work, not a duplicate of it.

**`docs/New_emu68.md` should be read before choosing a path.** It carries the
design record for where the Emu68 side is meant to go, and a display driver
that contradicts it would have to be rewritten.

# Acceptance criteria

- [ ] Path A or Path B chosen, with the reason written down
- [ ] The cost of the Display class's copy is measured on hardware
- [ ] A flip mechanism exists and `emu68gfx` uses it without the framebuffer
      address being assumed constant
- [ ] The boot UI hands over on a flip rather than on the first `Show()`
- [ ] Whether the firmware has to release the display is answered, not assumed

# Execution log

- 2026-08-17 — Opened at the user's request, as a deliberate exception to the
  freeze. Written around the A/B fork after the user pointed out that the
  mailbox's missing framebuffer tags may be irrelevant: they are the interface
  of a *firmware client*, which is exactly what a driver of our own stops
  being. That correction is what turned this from "implement the mailbox
  framebuffer tags" into a decision between two projects.
