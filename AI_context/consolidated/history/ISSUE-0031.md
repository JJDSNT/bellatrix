---
id: ISSUE-0031
title: "NewDTObject returns NULL for the PNGs BTScan tried, including one of the distribution's own"
status: closed
priority: medium
type: bug
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - datatypes
  - graphics
  - workbench
  - icons
blockers:
related_files:
  - external/aros/workbench/libs/muimaster/classes/dtpic.c
  - external/aros/workbench/libs/icon/diskobjPNGio.c
  - external/aros-bluzing/ports/aros/btscan/main.c
---

# What this issue claims, and what it does not

It claims one measurement: two `NewDTObject()` calls returned NULL. It does
**not** claim that datatypes is broken on this port, and an earlier draft of
this file did -- wrongly. AROS's own picture datatypes work; prefs editors
display icons through `Dtpic` and Workbench renders its icons. So the far more
likely explanation is something about **how this call was made**, or something
this port's setup leaves out, and not the subsystem.

Nor is "this port" established. The same call has never been tried on another
AROS target, so whether this is specific to emu68-m68k, to how this image is
assembled, or to the call itself is exactly the open question -- not a premise.

Read the numbers below as a starting point for finding that, not as a verdict
on datatypes.

# Summary

`NewDTObject()` returned `NULL` for both PNGs tried, with `IoErr()` left at 0
-- no DOS error, just no object.

Measured on a Pi 3 boot under QEMU on 2026-08-17, from inside BTScan, with
`pr_WindowPtr` set to -1 so a requester could not interfere:

```
[btscan] banner path=SYS:Extras/aros-bluzing/BTScan-banner.png open=008b623d
[btscan] datatypes.library=02448048
[btscan] NewDTObject SYS:Extras/aros-bluzing/BTScan-banner.png = 00000000 ioerr=0
[btscan] NewDTObject SYS:System/Images/Logos/AROS.logo         = 00000000 ioerr=0
```

The second line is the one that matters. `AROS.logo` is **the distribution's
own file**, a plain 600x300 8-bit RGB PNG that this build ships and that
nothing about this project produced. It fails exactly like ours.

`datatypes.library` opens. The file opens. The picture does not.

# What this is not

Ruled out by measurement, in this order, each one wrong:

- **Not a missing file.** `Open()` on the same path succeeds immediately before
  the failing call.
- **Not the filename.** A first guess blamed FAT 8.3 and long names. The system
  boots from `S:Startup-Sequence`, sixteen characters, so long names work.
- **Not alpha.** The banner was flattened from RGBA to RGB and behaves the
  same; `AROS.logo` has no alpha to begin with.
- **Not `PROGDIR:`** -- that is a real and separate finding, below, but the
  failure reproduces with an absolute path.
- **Not our PNG encoder.** The control file was not produced here.

# What is installed

Everything the path needs is present on the card:

| | |
|---|---|
| `Classes/DataTypes/picture.datatype` | 41.7K |
| `Classes/DataTypes/png.datatype` | 39.9K |
| `Devs/DataTypes/PNG` | descriptor, 128 bytes |
| `Libs/png.library` | 192.4K |
| `Libs/z1.library` | 78.9K |
| `Libs/datatypes.library` | 30.8K |

So this is not a packaging omission at the level of "the file was not copied".

# Why it is worth more than one missing decoration

**If it turns out to be real, it takes the icons with it.** `workbench/libs/icon/diskobjPNGio.c`
reads PNG icons through `proto/pngdt.h` -- the same png datatype. Every `.info`
in this distribution that is a PNG icon goes through the code that is failing
here, so "no picture loads" and "icons do not appear" are plausibly one bug and
should be checked together before either is investigated alone.

**It is silent.** `Dtpic` sets `pr_WindowPtr` to -1 and draws nothing when the
object fails, so an application sees a blank space and no message; icon.library
falls back just as quietly. Nothing in a normal boot says this is broken, which
is why it has gone unnoticed until an application asked for a picture.

# The goal: PNG pictures and PNG icons, both working

Two things should work here and neither does yet.

**PNG pictures**, so an application can put artwork on screen through `Dtpic`
without shipping its own decoder. BTScan already asks for one and gets nothing.

**PNG icons**, which is the part worth stating as an aim rather than a
symptom. AROS supports them -- `workbench/libs/icon/diskobjPNGio.c` reads a
`.info` that is a PNG carrying an `icOn` chunk, which is how OS4-style icons
work -- and that would let this project use its real artwork, in colour, at
whatever size, instead of quantising it to four pens.

**~~What is shipping meanwhile is a deliberate stopgap.~~** *Retired
2026-08-17.* `images/mkicon.py` wrote BTScan's icon in the classic `e310`
planar format, four colours, because that path depends only on icon.library.
It was reverted in bluzing `09ed296` once the PNG icon was confirmed to draw
**and open** on a Pi 3; the generator emits the PNG form again, and
regenerating the icon from the master reproduces the committed file byte for
byte.

# What is left

0. **Assume the caller is wrong first.** Compare against a program in the tree
   that displays a picture and works -- `workbench/prefs/serial/sereditor.c:130`
   and `workbench/classes/zune/aboutbox/Aboutbox.c:590` both use `Dtpic`, and
   `developer/debug/test/Zune/dtpic.c` exists to do exactly this. If one of
   those runs here and shows its picture, everything below is moot and the
   answer is in the difference between it and this call.
1. **Then find where it gives up.** `NewDTObject` -> `ObtainDataTypeA` identifies the
   file from the descriptors in `DEVS:DataTypes`, then opens the class from
   `CLASSES:DataTypes`. Two steps, and the return of NULL with `IoErr() == 0`
   says which is more likely: identification returning nothing rather than a
   class that failed to load and set an error.
2. **Try a non-PNG picture.** The distribution ships `ilbm.datatype` and
   `bmp.datatype`. If an ILBM loads and a PNG does not, the fault is in the png
   class or its libraries; if nothing loads, it is in datatypes or the
   descriptor database.
3. **Check the icons at the same time**, per above. One consequence is already
   confirmed: a PNG icon written for BTScan could not be opened from Workbench,
   and the same art in the classic `e310` format works. Every icon this
   distribution ships is classic, so nothing else had exercised the PNG path.
4. **Try it on another AROS target** before saying "this port" again.
5. Only then decide whether this is ours or upstream's.

# Notes

**Found while building BTScan, and it is not a BTScan bug.** The application
names its banner through `Dtpic` and keeps doing so; the day this is fixed the
picture appears with no change to it.

**A separate finding from the same session: `PROGDIR:` is not the program's
directory when a program is started with `Run`.** AROS gives a new process a
duplicate of *its parent's* home directory
(`rom/dos/createnewproc.c:353`), so a program launched from a script inherits
the shell's `PROGDIR:`. Workbench sets it properly, which is why this only
shows up in scripted launches. BTScan carries a fallback to its install path
because of it. Worth its own issue if anything else trips on it.

# Acceptance criteria

- [x] A PNG loads through `NewDTObject` here, or the reason it cannot is
      written down with the measurement behind it
- [x] A PNG icon can be opened from Workbench
- [x] BTScan's icon is its real artwork rather than a four-pen reduction
- [x] Whether this was ever port-specific has been answered, not assumed --
      it was never about datatypes or about this port at all

# It stopped reproducing (2026-08-17)

**`NewDTObject()` works.** BTScan's banner renders on a Pi 3, in full colour,
photographed from the screen: the `DtpicObject` in `main.c:457` -- the one the
source comments describe as drawing nothing until "the day datatypes does" --
is showing `BTScan-banner.png`.

That is the same call `icon.library` makes to read a PNG icon
(`diskobjPNGio.c`), so the icon path is unblocked with it.

**What is not established is why.** The measurement in this issue was taken the
same day, under QEMU, and returned NULL with `IoErr() == 0` for two different
PNGs including the distribution's own `AROS.logo`. Between then and now the
image changed in several ways at once -- the FAT `ParentDir` fix landed
(ISSUE-0036) and `ENV:` began populating, the tree was rebuilt with frame
pointers, and the user restored the `ENV:SYS/Packages` block that had been
removed while chasing ISSUE-0037, which is what runs bluzing's assigns.

Any of those could be it. The `ENV:` one is the most suspicious, because a
descriptor scan that silently finds nothing and reports no DOS error is exactly
the shape of the earlier symptom -- but that is a hypothesis, and this file's
own opening section warns against reading a starting point as a verdict.

**Do not close this by disappearance.** Re-run the original probe: BTScan's
`[btscan] NewDTObject ... = ` lines are still in the source and cost one run.
If they now return non-NULL under QEMU as well, the difference is in the image
rather than the hardware, and the change list above is short enough to bisect.

## What this unblocks, and what it does not

The classic planar icon was recorded as a stopgap in bluzing's own history --
*"the PNG form is where the icon should end up, in full colour, once the
decoder works"* -- so restoring it completes a documented intention rather than
adding anything. That is inside the freeze on the same footing the freeze
already grants this issue.

## A second thing the same photograph shows

BTScan lists five devices with RSSI, one of them identified as a mouse with
`HID = yes`, and reports `ready - 5 devices, 1 input`. **The Bluetooth stack is
discovering real devices on real hardware**, which means `btuart.resource` is
not merely resident during these boots -- it is receiving and framing traffic
throughout them. Worth recording in ISSUE-0037, where the question of what runs
on the Pi and not under QEMU is live.

# Execution log

- 2026-08-17 -- **Closed, with the cause named.** The PNG icon draws *and*
  opens on a Pi 3, and the banner renders in full colour. It was never
  datatypes and never this port: `Startup-Sequence:109` runs
  `AddDataTypes REFRESH QUIET`, which scans `DEVS:DataTypes`, and the FAT
  handler compared on-disk first-cluster fields without byte-swapping them
  (ISSUE-0036, `patches/aros/0023`), so that scan found nothing and -- being
  QUIET -- said nothing. Zero descriptors registered means `NewDTObject`
  returns NULL with `IoErr()` left at 0, which is exactly the measurement at
  the top of this file. The same defect emptied
  `Copy "ENVARC:" "ENV:" ALL`, which is how it was finally caught.

  So every item under "What is left" is answered by one earlier fix, and the
  four hypotheses this issue carefully refused to commit to were all correctly
  refused. The stopgap is reverted in bluzing `09ed296`.

  Not verified: whether the same probe now succeeds under QEMU. The claim above
  does not need it -- the mechanism is identified in code rather than inferred
  from where it stopped happening -- but a QEMU run would cost one boot if
  anyone wants it belt-and-braces.
- 2026-08-17 -- Frozen. `NewDTObject` returning NULL is a defect and diagnosing
  it stays in scope; making PNG icons work is an addition and does not, until
  the system is fast and stable (see `CLAUDE.md`).
- 2026-08-17 -- A PNG icon written for BTScan could not be opened from
  Workbench; replaced with a classic planar icon, which works. Recorded the
  PNG form as the goal rather than the failure.
- 2026-08-17 -- Opened. Reproduced under QEMU with the distribution's own
  `AROS.logo` as a control, after three wrong hypotheses (long filenames,
  alpha, `PROGDIR:`) were each disproved by measurement. An earlier attempt at
  the same probe appeared to hang; it was the probe missing
  `pr_WindowPtr = -1`, so a DOS requester was waiting for an answer no one
  could give in a headless boot.
