---
id: ISSUE-0043
title: "Drive the VideoCore display ourselves, instead of borrowing a framebuffer from the firmware"
status: doing
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

# The boot UI hold, and what its timestamps taught (2026-08-18)

The splash now holds the display from the first Show after Wanderer starts
until the desktop is drawn, which is what the fbgfx port made cheap: that
driver refreshes from each bitmap's own buffer, so suppressing the copy leaves
the splash up while the desktop is assembled behind it.

**The release mechanism took six attempts, and the log finally explained why
only one shape works.** A boot that came out right:

```
00:53.913  holding the display
01:12.633  hold armed: icons
01:12.653  hold released: settled
01:12.753  display takeover
```

Twenty milliseconds between armed and settled. The quiet window -- 1.5 s of no
drawing -- was already satisfied long before the signal arrived, so waiting for
the drawing to settle contributed **nothing** to the timing. That was the
explanation offered at the time and it is wrong.

What actually matters is the hundred milliseconds after it. The release only
marks the screen as owing a repaint; the copy happens on the next
`fbDoRefreshArea()`, which is a real drawing operation. Copying at the signal
-- which the "granular" version did, synchronously inside `set_stage()` --
is about 120 ms too early, and the screen title bar is not in the bitmap yet.
Copying on the next draw catches it.

So the working shape is: **decide in one place, copy on the next draw.** Not
because deferral is elegant, but because the deferral is what puts the copy
after whatever finishes the screen.

**The risk this carries is real and is not hypothetical.** An earlier boot
measured seventy seconds between release and repaint, because nothing drew in
between. The 30-second cap bounds it but does not fix it, and the failure mode
is a splash sitting over a finished desktop. If that recurs, the answer is not
to copy sooner -- that is the bug this section describes -- but to find what
draws the screen bar and wait for that specifically.

**What still has no explanation**: what draws the bar in those 100 ms.
`CreateScreenBar()` runs at screen creation, before `Show()` and therefore
before the hold begins, so the obvious answer is ruled out. `RenderScreenBar()`
is reached only from there, from `ActivateWindow()` and from the input handler;
there is no invalidate-and-redraw entry point in the API at all, which is why
five attempts at forcing a redraw failed. `ShowTitle()` and `ActivateWindow()`
both queue through `DoASyncAction()` and the queue is drained by the input
handler, so neither runs on a boot with no input.

# vc4gfx on real hardware: it reaches the silicon and declines (2026-08-18)

The driver ported, linked and booted on a Pi 3. Under QEMU it stood down --
`no HVS found (ID=0x00000000)` -- which was the right answer there. On hardware
it is a different machine entirely:

```
[VC4HVS] ID=0x76726464 DISPCTRL=0xff0f0c9a DISPSTAT=0x00000000 DISPLSTAT=0x00000030
[VC4HVS] ch1: LIST=0000 LACT=0000 CTRL=38047880 BKGND=00000001 STAT=ee430380
[VC4HVS] PV2 @ 0xf2807000: CTRL=05701700 VCTRL=03000000 STAT=44040000
[VC4HVS] fw kernel @4084: 00fceb07 f8ede307 fd054800 ...
```

`0x76726464` is "vrdd" in ASCII. It reads the HVS identification register, all
three channels, all three PixelValves and the firmware's own filter kernel, and
channel 1's STAT advances between reads -- a live display, being scanned out by
the firmware, correctly observed by our driver.

**Then it refuses the takeover**, and the reason is exact:

```
[VC4HVS] expecting fb: phys=0x00000000 pitch=0 1920x1080
[VC4HVS] takeover: HDMI channel not usable (CTRL=38047880 head=0)
```

`vc4_hvs_takeover()` (`vc4gfx_hvs.c:652`) requires three things of the HDMI
channel before it will inherit the firmware's framebuffer plane:

```c
if (!(ctrl & HVS_DISPCTRLX_ENABLE) || head == 0 || head >= HVS_DLIST_WORDS)
```

`head` is zero. There is no display list to inherit at the moment the driver
looks.

## Two findings, and neither is "the firmware will not let go"

**The list exists; the driver looks too early.** A second dump later in the
same boot shows `ch1: LIST=64060000 LACT=64060000`. The firmware publishes its
display list, just not by the time a resident at priority 9 runs. That reframes
the largest unknown this issue was opened with: the question is not whether the
firmware releases the display, it is *when* there is something to take.

The same later dump reports `head=1678114816`, which fails the other half of
the same test (`head >= HVS_DLIST_WORDS`). A plausible reading is that
`HVS_DISPLACT` is not the register this check wants, or wants masking; both are
cheap to establish against the second dump rather than guessed at.

**The driver does not know where the framebuffer is.** `expecting fb:
phys=0x00000000 pitch=0` while Emu68 reports `[BOOT] Framebuffer @ 3e7fe000`
and a 1920x1080 display. On arm-native it would learn this from the firmware
mailbox; here the address is already published by our kernel as
`KATTR_FrameBuffer` and its pitch companion (added for fbgfx), and nothing
connects the two. That is the smaller of the two gaps and the more obviously
fixable.

## What this does to the paths

Path B -- driving the pipeline directly -- is no longer hypothetical. The
hardware answers, the registers read correctly, and the only thing between here
and a display list of our own is inheriting or building one. The choice between
A and B does not need making on faith any more.

# Inherited from ISSUE-0021: the boot time doubled, and nobody knows which change did it

Carried here when that issue closed, because it is not a boot UI question.

The user observed the boot going from roughly one minute to two. Two things
changed in the same period and both are behind a switch:

* **`GFX_BACKEND`**, emu68gfx to fbgfx. The new driver refreshes **by area** --
  one copy per drawing operation, each with its own bounds arithmetic -- where
  the old one let the generic Display class copy the whole screen once per
  `Show()`. Many small copies against few large ones is not obviously the
  cheaper trade, and it has never been measured.
* **`BELLATRIX_FRAME_POINTERS`**, turned on the same day. Documented at the
  time as costing "an unmeasured amount of speed", and `CLAUDE.md` records that
  numbers taken with it on do not compare with the 153 runs in
  `out/boot-timing.jsonl`.

Two independent switches, so four combinations; and since flipping the backend
only needs a relink while flipping frame pointers needs a reconfigure, the
cheap order is to fix the latter and vary the former first.

**Not measured by decision.** The A/B was offered and declined -- the work at
hand was the boot UI, not the clock. Recorded so that "the boot got slower" has
a date, two named suspects and a method attached rather than becoming folklore.

Note this is the same number the issue already asks for above: what the Display
class's copy costs. Answering that answers half of this.

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

- [x] **Path C done**: `arch/aarch64-raspi/hidd/fbgfx` builds, boots and drives
      the display on this target, switchable by one line (`GFX_BACKEND`)
- [x] The boot UI hands over when the desktop is drawn rather than on the first
      `Show()` -- reached without a flip, see below
- [ ] Path A or Path B chosen, with the reason written down
- [ ] The cost of the Display class's copy is measured on hardware
- [ ] A flip mechanism exists and the driver uses it without the framebuffer
      address being assumed constant
- [ ] Whether the firmware has to release the display is answered, not assumed

## Where this stands (2026-08-18)

**What landed is path C and only path C.** The driver AROS already had, ported
and running: 41 symbols in the ELF, a full Wanderer desktop with correct
colours, and two adaptations that cost two functions --
`initFBGfxHW()` reading `KATTR_FrameBufferDepth` instead of assuming 32bpp
(`patches/aros/0031`), and the display class forwarding
`aHidd_PixFmt_SwapPixelBytes` for a 5:6:5 halfword on a big-endian CPU.
`fbDoRefreshArea()` needed nothing: it was already format-agnostic.

**No VideoCore register has been written.** The framebuffer still arrives
ready-made from Emu68 and nothing of ours asks the hardware for anything, which
is the whole distinction this issue was opened to keep straight. Paths A and B
are untouched, and ISSUE-0012's hardware cursor still waits on B.

**The boot UI hand-off came for free, and not the way this issue predicted.**
It was written expecting a flip to make it possible. It did not need one: fbgfx
refreshes from each bitmap's own buffer, so suppressing that copy leaves the
splash on the framebuffer while the desktop is assembled behind it -- double
buffering with the bitmap as the back buffer, already present in the driver
AROS ships. ISSUE-0021 and ISSUE-0034 closed on it.

**What is now worse, not better, and unmeasured.** The boot time roughly
doubled, with this port and the frame pointers as the two suspects (above).
Until that is attributed, "the port paid for itself in speed" is not a claim
anyone here can make.

# Execution log

- 2026-08-18 -- **The list at word 0 is a bare END.** With `head == 0` accepted,
  the dump walked it: `[0000] 0x80000000 END`. One word, no planes -- that
  channel draws background and nothing else. And the channel is live:
  `DISPSTATX` mode reads RUN and its line counter moves between dumps
  (a0034290, a00383b6). Something is on the wire that this list does not
  describe.

  `patches/aros/0040` stops asking DISPLIST/DISPLACT where the list is and
  scans dlist RAM for the one thing a display list cannot fake -- a word
  holding our framebuffer's address, alias bits masked. Read-only, bounded to
  the 4096 words already proven readable (the firmware's filter kernel comes
  back from word 4084 intact). The DISPBASEX windows tile a region reaching
  ~20736 words -- ch2 [0,2047], ch1 [2048,15503], ch0 [15504,20735] -- which is
  a real hint that dlist RAM is larger than assumed, but the far end of it
  would be peripheral offset 0x416400, past the 0x6000 the SCALER block is
  documented to occupy. Widening the scan is a separate decision.

- 2026-08-18 -- **The endian fix landed and the dump became readable.** On the
  Pi 3: `ID=0x64647276`, `ch1 CTRL=80780438` (ENABLE | 1920 << 12 | 1080), PV2
  reporting real 1080p timings (hfp 88, hsync 44, hbp 148, vfp 4, vsync 5, vbp
  36), and the firmware's PPF kernel reading back as a symmetric 11-tap filter.
  Every register the driver touches is now correct.

  `head=0` survived, and it is not a failure. `LACT=0000` is where a firmware
  *boot* display list lives -- the bottom of dlist RAM. arm-native never sees it
  because it asks the firmware for a framebuffer first, and that FBALLOC makes
  the firmware rebuild its list mid-RAM (word 0x664, which is exactly what the
  second dump in the previous run showed once a mode had been set). This port
  asks for nothing, so the boot list is still what is on the channel. Linux's
  vc4 agrees from the other side: it reserves the first words as "the
  bootloader's dlist" rather than treating word 0 as free. `patches/aros/0039`
  drops the `head == 0` rejection from the takeover, the probe and the dump; the
  real test was always walking the list for a plane whose PTR0 is our
  framebuffer, and a head of zero that leads nowhere still declines one entry
  later.

- 2026-08-18 — vc4gfx reached the silicon and read every register backwards.
  The takeover declined on every Pi 3 boot with `CTRL=38047880 head=0`, and
  0x38047880 is 0x80780438 byte-reversed -- `ENABLE | 1920 << 12 | 1080`, the
  channel enabled and running the mode we expected. `HVS_ID` told the same
  story: 0x76726464 where the header records 0x64647276. `hvs_rd()`/`pv_rd()`
  dereference a `volatile ULONG *` and return it as-is, which is right on
  arm-native and wrong here; the registers are little-endian like every other
  BCM283x peripheral, as `vc4gfx_dma.c` and this port's `mbox.resource` already
  say in their own code. `patches/aros/0038` wraps the four accessors in
  `AROS_LE2LONG`/`AROS_LONG2LE` -- a no-op on a little-endian host -- and, in
  the same failure path, teaches the fb-plane search to recognise RGB565: the
  surface this port is handed is 16-bit, so a takeover that only knew RGBA8888
  could never have inherited its own framebuffer. Neither half is confirmed on
  hardware yet.

  The same day, a build trap: mmake does not expand make variables in `#MM`
  lines. `#MM- kernel-link-emu68-m68k : ... $(GFX_KOBJ) ...` was a dependency
  on a metatarget literally named `$(GFX_KOBJ)`, which does not exist and which
  mmake drops silently -- so no gfx driver was ever a dependency of the kernel
  link, and vc4gfx was being linked from whatever object happened to be on
  disk. `aros/arch/m68k-emu68/boot/mmakefile.src` now names all three drivers
  literally and builds all three; only the one `GFX_BACKEND` selects is linked.

- 2026-08-17 — Opened at the user's request, as a deliberate exception to the
  freeze. Written around the A/B fork after the user pointed out that the
  mailbox's missing framebuffer tags may be irrelevant: they are the interface
  of a *firmware client*, which is exactly what a driver of our own stops
  being. That correction is what turned this from "implement the mailbox
  framebuffer tags" into a decision between two projects.
