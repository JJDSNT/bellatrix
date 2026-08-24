---
id: ISSUE-0057
title: "Icon and window rendering regressed on real hardware, and the cause is not identified"
status: open
priority: high
type: bug
owner: unassigned
created_at: 2026-08-24
updated_at: 2026-08-24
tags:
  - performance
  - regression
  - usb
  - sdcard
  - raspberry-pi-3
blockers:
related_files:
  - AI_context/issues/ISSUE-0056.md
  - AI_context/issues/ISSUE-0040.md
  - aros/arch/m68k-emu68/soc/usb/usb2otg
  - aros/arch/m68k-emu68/soc/sdcard
---

# Summary

Observed on a Raspberry Pi 3, 2026-08-24: drawing icons and windows is slower
than it was. Reported from use rather than from a measurement, which is a
perfectly good way to notice a regression and not a way to diagnose one.

**No measurement exists yet, for either the old state or the new one.** That
is the first thing this issue needs and the reason it cannot be assigned a
cause.

# The two candidates

**USB.** The card now boots with `usb2otg.device` enabled, and it takes the
SOF interrupt unconditionally at 1 kHz. On this port every guest interrupt is
an m68k exception through Emu68's IPL bridge -- ARM peripheral -> INTF.IPL ->
level 6 -> `Platform_Autovector()` -> `Dispatch()` -> `krnRunIRQHandlers()`.
A thousand of those a second compete with whatever is drawing. This is exactly
the question [ISSUE-0056](ISSUE-0056.md) was opened to settle, and it was
opened before this was noticed.

Note that USB was **off** on every earlier card. If the comparison being made
is against a build without USB, USB is not merely a candidate, it is the
difference.

**The SD card.** A `glinfo` run on hardware produced this, and it has not been
explained:

    [SDHost00] 2048 cmds, 16212 KB, 4096 KB in 708336 ms = 5 KB/s
    [SDHost00]   command total 478 ms of which transfer 439 ms

708 seconds of wall clock against 478 ms of command time, with the two
following samples at 630 ms and 1079 ms and entirely normal. Either the
counter wrapped, or the driver spent eleven minutes somewhere outside the
command path. Those are very different findings and the log cannot separate
them.

Icon drawing reads from disk -- `.info` files, images, fonts -- so a driver
that stalls intermittently would look exactly like slow rendering.

# What has changed recently, as candidates rather than suspects

- USB enabled on the card at all, with `usb2otg` (SOF at 1 kHz);
- `patches/aros/0038`, FAT cache buffers aligned for DMA;
- `patches/aros/0037`, a larger FAT cache;
- the SD backend is SDHOST with DMA (`SDCARD_BACKEND := sdhost`);
- `patches/aros/0055`, the Exec allocator's alignment checks -- touches every
  allocation, so it belongs on this list even though nothing suggests it.

# How to separate them

Cheapest first, and the first one probably settles it:

1. **Boot the same card with `BELLATRIX_USB=0`.** Same build, same everything,
   USB absent. If the rendering recovers, it is USB and ISSUE-0056 becomes
   urgent rather than academic.
2. If it does not recover, boot a build from before these changes and confirm
   the regression is real rather than remembered -- the honest possibility
   that nothing changed and the machine was always this slow has not been
   excluded.
3. Instrument the SD driver's wall-clock accounting to explain the 708
   seconds. It is either a wrapped counter, which is a reporting bug, or a
   real stall, which is the whole answer.
4. Only then look at the FAT cache and allocator changes.

# Why "slower" needs a number before anything else

Nothing here has a baseline. `out/boot-timing.jsonl` holds 153 runs but they
predate frame pointers and cannot be compared against current builds
(CLAUDE.md says so explicitly). Boot-to-`hold released` is measured routinely
and is **not** what regressed -- it is still ~1:03 under QEMU. Whatever got
slower is after the desktop appears, and nothing measures that yet.

A stopwatch on "open a drawer with N icons" would be enough to make this
comparable between two boots, and without something like it every conclusion
here is going to be somebody's impression.
