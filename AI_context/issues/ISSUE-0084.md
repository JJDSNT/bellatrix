---
id: ISSUE-0084
title: "REGRESSION: DPaint stopped opening its screen once chipset DMA was really enabled"
status: open
priority: critical
type: regression
owner: unassigned
created_at: 2026-08-31
updated_at: 2026-08-31
tags:
  - regression
  - rigel
  - amigavideo
  - performance
blockers: []
related_files:
  - src/amiga/bus.c
  - external/aros/arch/m68k-amiga/hidd/amigavideo/amigavideo_blitter.c
  - external/aros/rom/graphics/
---

# What worked, and when

Earlier on 2026-08-31, Deluxe Paint IV started from its Wanderer icon:

- its mode requester opened -- visible in the log as a display retarget,
  `[VC4HVS] takeover: ACTIVE - list 3584, out 1920x1080, fb 640x480 ->
  1440x1080 at 240,0`, so the requester is a 640x480 screen of its own;
- the user chose a PAL mode and pressed OK;
- amigavideo opened a 320x256x5 Amiga screen -- `setmode: (320x256x5) bpr=40
  fu=3` -- and `blit_fillrect` / `blit_copybox` ran.

That is the whole chain, and it ran repeatedly across several runs.

# What happens now

DPaint opens nothing. In the last run it was started from `S:Startup-Sequence`
rather than from its icon, which removes the double-click from the question
entirely, and still:

- no `[VC4HVS] takeover` after the boot's own two;
- no `setmode:`;
- no screen of any kind, Amiga or vcgfx.

The machine is alive: the pointer moves to the last line of the log, and the
LIVE probe shows varied PCs.

# CORRECTION: the double-click works

Written above and reasoned from at length: the click was delivered but its two
halves did not arrive close enough together to be one gesture, because of the
USB interrupt storm. That is wrong, and the user's own observation settles it:

> "o duplo clique so nao funcionava no dpaint, eu conseguia abrir as pastas"

Folders open. The double-click works. The storm is real and ISSUE-0078 still
stands, but it is not why DPaint does not start, and the Startup-Sequence run
had already removed the click from the question anyway.

What is left is much sharper: **DPaint alone fails, and it fails before it
opens anything.** No screen of any kind is created, so it dies earlier than its
own mode requester.

# What DPaint does before it opens a screen

It opens disk fonts. `tools/dpaint/install.sh` stages a `fonts` drawer beside
the binary -- `fonts/dpaint.font` and a `fonts/dpaint/` directory -- and the
application asks diskfont for them at startup. Wanderer does not: it draws
folder windows with the ROM font, which is exactly why folders still open while
DPaint does not.

That is the shape of a trap this project has already paid for once, recorded in
the memory as "o cartao envenena-se sozinho": font indexes written by the guest
onto the FAT volume, plus a fat-handler without its endian fix, and every
subsequent boot hangs where those files are read back. `read_block_buffered` at
15% of samples is the fat-handler churning, which is consistent with it.

The same shape covers anything else DPaint wrote to its own drawer on an
earlier run -- settings, the mode it was given -- coming back corrupt.

It fits the one fact that no other explanation does: **it worked repeatedly
this morning and then stopped**, with nothing in between that touched DPaint's
files. A volume that poisons itself after a successful run does exactly that.

# Where the time goes

129 LIVE samples over the run:

```text
27 (21%)  inside amigavideo.hidd      (module base <= 0x043d4aba)
36 (28%)  emu68_DispatchFrame, scan_bank, intc_read
20 (15%)  read_block_buffered
```

A fifth of the machine inside the chipset display driver, with no Amiga screen
open at all. The 28% in interrupt dispatch is the old USB driver's SOF storm
(ISSUE-0078) and predates this.

The exact function inside amigavideo is **not** established: the module's load
base is not in the log, and `waitvblank()` -- the obvious suspect from
ISSUE-0081 -- is called only from `resetmode()`, which is not where the hot PCs
appear to fall. Do not assume it without pinning the base.

# The change that lines up

`c037097` set DMACON's master enable at bring-up, because nothing else did and
the chipset composed nothing without it (ISSUE-0083). It worked: the census went
from `flags=00, non-bg=0` forever to `flags=0c` and `something is drawing`.

But it also made a great deal of previously inert code real, and the blitter is
the part that matters here. `amigavideo_blitter.c` brackets every operation
with graphics.library's `OwnBlitter()` / `WaitBlit()`, and `WaitBlit()` on m68k
spins reading DMACONR until BBUSY (bit 14) clears. With the master enable off,
the blitter never started, BBUSY was never set, and every `WaitBlit()` returned
on its first read. Every blit in the earlier, working runs was a no-op that
nobody waited for.

Now the blits are real, and two things follow that did not before:

1. **They take Amiga time.** Rigel is cycle-exact and runs at 99% of realtime
   (`3545321 CCK/s`), so a blit costs what a blit costs on an A500.
2. **The wait is a trapped read.** `$dff002` is inside the
   MACHINE_REGION_EXTERNAL custom aperture, so each iteration of `WaitBlit()`'s
   spin is a data abort into Emu68's fault handler and on into Rigel -- not a
   load. A spin loop built for a bus cycle is now paying a fault per turn.

Neither is a bug in the DMA change; both are consequences of it that the rest
of the system has never had to survive before. And a blit that never reports
completion at all -- BBUSY that does not clear, or the BLIT interrupt that
`amigavideo_chipset.c:1065` waits on never arriving -- would show as exactly
this: a driver that burns CPU and an application that never gets its screen.

# The other candidates, so they are not forgotten

- `patches/aros/0092` (the `SetPointerPos` guard) landed in the same window. It
  turns a fatal wild write into a no-op, so it cannot stop a screen opening --
  but it is in the window and is named here rather than assumed innocent.
- `vcgfx_denise.c` runs a task at priority -5 doing `Delay(10)`; it did nothing
  at all in the last run (no `[VideoCoreGfx:Denise]` lines) because BPLEN was
  never set.
- The frame descriptor gained a field and DMACON writes are now shadowed in
  `amiga_bus_write()` -- one comparison on a path that already existed.

# Where to start

Three experiments, free, in this order -- the first two do not need a build.

**Replace DPaint on the card with a clean copy.** `out/dpaint/` holds what the
extractor staged, untouched by any boot. Delete `DPaint/` on the card and copy
that in its place. If it then starts, the volume poisoned itself and the
question becomes which write did it -- which is the fat-handler endian work,
not the display work.

**Rename `Devs/Monitors/AmigaVideo` on the card.** If DPaint's requester then
opens, the block is inside amigavideo and the profile above says where to look.
If it still does not open, amigavideo is cleared and the cause is earlier, in
DPaint's own startup.

**Then revert the master enable** (comment out `amiga_chipset_enable_dma()`)
and run again with amigavideo present. If the requester comes back, the
regression is confirmed as the second-order effect of real DMA rather than
anything about the register itself -- and the fix is not to turn DMA off again
but to make `WaitBlit()` survivable, which is the chip-bus work ISSUE-0071
already describes.

Do not conclude from one run: this path has been intermittent all along
(CLAUDE.md, "Measuring a boot").
