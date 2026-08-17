---
id: ISSUE-0031
title: "No picture loads through datatypes on emu68-m68k: NewDTObject returns NULL for every PNG"
status: backlog
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

# Summary

`NewDTObject()` returns `NULL` for every PNG tried on this port, with `IoErr()`
left at 0 -- no DOS error, just no object.

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

**It very likely takes the icons with it.** `workbench/libs/icon/diskobjPNGio.c`
reads PNG icons through `proto/pngdt.h` -- the same png datatype. Every `.info`
in this distribution that is a PNG icon goes through the code that is failing
here, so "no picture loads" and "icons do not appear" are plausibly one bug and
should be checked together before either is investigated alone.

**It is silent.** `Dtpic` sets `pr_WindowPtr` to -1 and draws nothing when the
object fails, so an application sees a blank space and no message; icon.library
falls back just as quietly. Nothing in a normal boot says this is broken, which
is why it has gone unnoticed until an application asked for a picture.

# What is left

1. **Find where it gives up.** `NewDTObject` -> `ObtainDataTypeA` identifies the
   file from the descriptors in `DEVS:DataTypes`, then opens the class from
   `CLASSES:DataTypes`. Two steps, and the return of NULL with `IoErr() == 0`
   says which is more likely: identification returning nothing rather than a
   class that failed to load and set an error.
2. **Try a non-PNG picture.** The distribution ships `ilbm.datatype` and
   `bmp.datatype`. If an ILBM loads and a PNG does not, the fault is in the png
   class or its libraries; if nothing loads, it is in datatypes or the
   descriptor database.
3. **Check the icons at the same time**, per above.
4. Only then decide whether this is ours or upstream's.

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

# Execution log

- 2026-08-17 -- Opened. Reproduced under QEMU with the distribution's own
  `AROS.logo` as a control, after three wrong hypotheses (long filenames,
  alpha, `PROGDIR:`) were each disproved by measurement. An earlier attempt at
  the same probe appeared to hang; it was the probe missing
  `pr_WindowPtr = -1`, so a DOS requester was waiting for an answer no one
  could give in a headless boot.
