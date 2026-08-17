---
id: ISSUE-0033
title: "Deluxe Paint IV loads and exits without a word: the first real Amiga application tried on this machine"
status: backlog
priority: low
type: investigation
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - compatibility
  - graphics
  - screens
  - amiga
blockers:
related_files:
  - scripts/install-dpaint.sh
  - AI_context/issues/ISSUE-0023.md
---

# Summary

DPaint IV installs onto the card, is found, runs, returns, and says nothing.
Workbench is still there afterwards; nothing crashes; the serial console is
silent.

Measured under QEMU on 2026-08-17. Started from `S:User-Startup` with its
output redirected to a file so nothing could swallow a complaint:

```
SYS:Tools/DPaint >SYS:dpaint.out
```

The file is created and is **zero bytes**. Startup then continued and Wanderer
came up, so the program was loaded, ran and exited of its own accord.

`scripts/install-dpaint.sh` puts it there, from a DPaint IV floppy the user
supplies. Only the application, its icon and its font are taken; nothing from
the floppy's 1988 C:, L:, libs: or devs:.

# The likely reason, and why it is interesting rather than disappointing

**This machine has no Amiga chipset.** That is the whole premise of the port.
DPaint IV opens its own screen, and the modes it asks for are chipset display
modes -- lores, hires, HAM, EHB. If `OpenScreen` cannot give it one it has
nothing to draw on and no reasonable option but to leave, which is exactly the
shape of what was observed: loaded, silent, gone.

That would make this the same wall recorded elsewhere in this project, reached
from a new direction: an application asking for a display the graphics driver
does not offer.

# What is left

1. **Confirm it is the screen.** The cheapest evidence is a program that opens
   a screen and reports the failure, rather than inferring from silence -- or
   running DPaint on hardware, where a failure requester would at least be
   visible. It has only been seen headless.
2. **Find out which mode it asks for.** If it requests a specific chipset
   ModeID, nothing short of offering one will help. If it asks for something
   loose -- a depth and a size -- then the answer is what the graphics driver
   advertises, and that is ours to change.
3. **Try an application that does not open its own screen.** Something that
   runs in a Workbench window would separate "no chipset screens" from "no
   AmigaOS applications at all", which is a much bigger claim and one nothing
   here supports yet.

# Notes

**The value is the method, not the result.** There is now a repeatable way to
put a real Amiga application on the card and see what happens, which did not
exist before and which the boot-compatibility work will want again.

**Do not read this as "DPaint does not work".** It has been tried once, in an
emulator, with no way for it to report anything. What is established is that it
runs and exits quietly -- not why.

# Execution log

- 2026-08-17 -- Installed from a DPaint IV floppy and tried under QEMU at the
  user's request. Ran and exited silently, twice: once detached with `Run`,
  once synchronously with its output captured to a file that came back empty.
