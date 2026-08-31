---
id: ISSUE-0053
title: "Every boot loads Bluetooth firmware loaders for a radio this machine does not have"
status: open
priority: low
type: bug
owner: unassigned
created_at: 2026-08-24
updated_at: 2026-08-24
tags:
  - boot
  - bluetooth
  - startup-cost
  - diagnosis
blockers:
related_files:
  - external/aros/rom/bluetooth/firmware/
  - external/aros/rom/bluetooth/firmware/mmakefile.src
  - scripts/build-aros.sh
  - AI_context/issues/ISSUE-0030.md
---

# Summary

A normal boot to the desktop loads two Bluetooth firmware loaders that cannot
ever apply to this hardware:

    12:51:28 [InitResident] bluetooth.library: MakeLibrary 1 ms, calling init @ 0x04343f9c
    12:51:28 [InitResident] bthid.class:      MakeLibrary 0 ms, calling init @ 0x043730dc
    12:51:37 [InitResident] rtlbtv2.fwl:      MakeLibrary 0 ms, calling init @ 0x0431f430
    12:51:37 [InitResident] rtlbtv1.fwl:      MakeLibrary 0 ms, calling init @ 0x04320dd8

`rtlbtv1.fwl` and `rtlbtv2.fwl` are **Realtek** Bluetooth firmware loaders,
installed into `Devs/Bluetooth/FWLoaders/` at 9.3 KB and 9.1 KB. The Raspberry
Pi 3's radio is a **Broadcom BCM43438** on a UART, which is what
`Devs/Bluetooth/bthciuart.device` is for and why `build-aros.sh` builds
`kernel-bthciuart` explicitly.

Neither loader can initialise this radio. They are loaded on every boot
regardless, because `bluetooth.library` loads whatever is in the FWLoaders
directory, and these are what is there.

# Why they are there at all

Not a packaging mistake of ours, and not a choice: `rom/bluetooth/firmware/`
contains exactly two subdirectories, `rtlv1/` and `rtlv2/`. **AROS has no
Broadcom firmware loader.** So the FWLoaders directory cannot contain anything
useful to this machine — there is nothing useful to put in it.

That is worth stating separately from the waste, because it bounds what
Bluetooth on this port can currently be: the HCI transport exists
(`bthciuart.device`), and the firmware handshake for our chip does not. See
[ISSUE-0030](ISSUE-0030.md) for what the legacy port established about the
Bluetooth stack.

# What it costs

Small, and measured rather than assumed. Both report `MakeLibrary 0 ms`, so
the cost is two `LoadSeg`s of ~9 KB each plus their romtag scans, on a card
whose driver spends 7 ms per command ([ISSUE-0051](ISSUE-0051.md)). It is not
where the boot time is. It is listed because a boot that loads code which
cannot run is a boot carrying a variable for no return, and the standing
decision of 2026-08-17 is that speed and stability come before anything new.

# Seen alongside it

The `OpenLibrary` reporting added under `patches/aros/0040` also named a
second one in the same boot:

    12:51:27 [LDDemon] OpenLibrary("setpatch.library", 41) opened but returned NULL

Probably harmless here -- SetPatch patches a Kickstart this machine does not
have, which the 2026-08-15 AmigaOS 3.1 compatibility test already ran into --
but it is the same class of thing as the missing `dos64.library`, and nothing
had ever reported it. Worth keeping in view rather than acting on.

Note that this line depends on a diagnostic patch that may not survive; if
`0040` is dropped, this observation goes back to being invisible.

# What is not decided

Three options, none costed, and none of them urgent:

- **Leave it.** Two small loads, and the directory becomes correct by itself
  the day AROS grows a Broadcom loader.
- **Stop installing them on this target.** They are dead weight here and only
  here. It is a packaging change, not a code change, and it makes the boot
  carry one less thing that cannot work.
- **Stop loading Bluetooth at all until something needs it.** Larger, touches
  `bluetooth.library` and `bthid.class` too, and trades boot cost against a
  behaviour change. Out of scope while the freeze holds.

The second is the obvious one; it has not been done because nothing has
measured whether anything else expects those files to exist.

# Verification

A boot log with no `rtlbtv*.fwl` line in it, reaching `hold released: icons`
in the same time as now (~50 s under QEMU, from
`scripts/make-sdcard.sh` with no boot test).
