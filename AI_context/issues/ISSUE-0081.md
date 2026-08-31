---
id: ISSUE-0081
title: "amigavideo hangs in waitvblank during its own initialisation"
status: open
priority: high
type: defect
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-30
tags:
  - rigel
  - amigavideo
  - interrupts
blockers: []
related_files:
  - external/aros/arch/m68k-amiga/hidd/amigavideo/amigavideo_chipset.c
  - aros/arch/m68k-emu68/platform/platform.c
---

# Symptom

With `DEVS:Monitors/AmigaVideo` on the card the machine boots, hands the
display over, and stops:

```text
[BootUI] [00:02.950] display owned by a native driver
[InitResident] amigavideo.hidd: MakeLibrary 0 ms, calling init @ 0x043ca1c6
[LDDemon] OpenLibrary("amigavideo.hidd", 0) opened but returned NULL
[BELLATRIX:LIVE] pc=043cbf66 sr=2604 arm=0 ipl=0      <- and this, for ever
```

The same address every second for the rest of the log. `amigavideo.hidd`
initialised at `0x043ca1c6`, so the PC is `0x1da0` into it, and `OpenLibrary`
returns NULL because its init never completes.

The boot UI clock stopping at three seconds is **not** this: that is the
display handover, and it is expected. The hang is what follows it.

# Where

`amigavideo_chipset.c:73`:

```c
static VOID waitvblank(struct amigavideo_staticdata *csd)
{
    // ugly busy loop for now..
    UWORD fc = csd->framecounter;
    while (fc == csd->framecounter);
}
```

`csd->framecounter` is incremented only by `gfx_vblank`
(`amigavideo_chipset.c:1667`), the server the driver installs at line 1750
with `AddIntServer(INTB_VERTB, &csd->inter)`. It is called from
initialisation, at line 226, among other places.

So the driver spins waiting for a VERTB that does not arrive. A spin loop is
also the only shape that produces a PC frozen at one address, which is what
the liveness probe reports.

# It draws first, and that matters

```text
[BELLATRIX:RIGEL:CENSUS] something is drawing
[CENSUS] frame=89 256x256 pitch=4096 bg=00000000 non-bg=1184/1369 sum=e289a400
[CENSUS] background is now 00aaaaaa
[CENSUS] the picture went empty
```

1184 of 1369 sampled pixels differing from the background: **the producer
works**. This is the first time anything on this port has put a real picture
into Rigel's Denise. It then goes to flat Workbench grey and stops, which is
the hang.

# Why this matters beyond itself

Without this driver there is no producer at all for the classic chipset. An
application that draws through `graphics.library` -- which is every
well-behaved one, Demo Reel 3 included -- has its bitplanes rendered to the
VideoCore by `vcgfx`, and Denise is handed nothing. ISSUE-0079 and ISSUE-0068
both end here.

# What to check first

Paula's interrupts are delivered as of `e89a0bd`, and gated off the platform
path by `8235434`, so `INTB_VERTB` should reach `AddIntServer` chains. Two
things to establish, in this order:

1. **Is VERTB reaching the chain at all?** `platform/bcm283x/system_timer.c`
   already causes `INTB_VERTB` from the ARM timer, guarded by
   `if (SysBase && (IDNESTCOUNT_GET < 0))` -- so a caller holding `Disable()`
   stops it. A busy loop inside a Disable() would never see its own counter
   move and would deadlock exactly like this.
2. **Is the server installed by the time the wait runs?** `AddIntServer` is at
   line 1750 and `waitvblank` is called at 226. If initialisation waits before
   installing, nothing can ever move the counter.

The second is cheap to check by reading, and is the more likely of the two.
