---
id: ISSUE-0029
title: "SDHCI data timeout and a CMD/DATA reset on every boot, right after the card is sized"
status: backlog
priority: medium
type: bug
owner: unassigned
created_at: 2026-08-16
updated_at: 2026-08-16
tags:
  - sdcard
  - bcm283x
  - arasan
  - hardware
blockers:
related_files:
  - external/aros/rom/devs/sdcard/sdcard_bus.c
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_bcm2708bus.c
  - aros/arch/m68k-emu68/soc/sdcard/sdcard_bcm2708init.c
---

# Summary

On a real Pi 3, every boot logs the same four lines immediately after the card
reports its capacity:

```
[SDBus00] controller interrupt is being delivered
[SDBus00] MMC0: [29818MB Capacity]
[SDBus00] controller interrupt is being delivered
[SDBus00] SDCARD__SDBus__FinishData:    Timeout!
[SDBus00] SDCARD__SDBus__SendCmd: Controller failed to release inhibited bit(s).
[SDBus00] SDCARD__SDBus__SendCmd:  failed bit(s) = 00000002
[SDBus00] SDCARD__SDBus__SendCmd: Reseting SDHCI CMD/DATA
```

The boot then continues normally and the card is usable — DOS starts, the
distribution loads, the desktop comes up. So this is not a blocker; it is a
transfer that times out and is recovered by resetting the controller, once, on
every single boot.

It is unrelated to the Bluetooth work it was found alongside.

# What the log says

- `failed bit(s) = 00000002` is `SDHCI_PSTATE_DATINHIBIT`: the controller is
  still holding the **data** line inhibit when the next command is issued. Bit 0
  (command inhibit) is clear, so the command path is idle and only the data
  phase is stuck.
- The preceding `FinishData: Timeout!` says the driver had already given up
  waiting for that data transfer to complete, so the inhibit is the same
  transfer, seen a second time from the command side.
- The recovery — `Reseting SDHCI CMD/DATA` — works, which is why nothing
  downstream notices.

`rom/devs/sdcard/sdcard_bus.c:899-902` is the generic code that reports and
recovers; the timeout itself comes from the same file's `FinishData`.

# Why it is worth a look despite being harmless today

**It costs a controller reset on the boot path.** The reset is a blunt recovery:
it discards controller state, and the driver then re-establishes it. Doing that
once per boot is cheap, but it means the first data transfer of every session is
known to fail, and nothing has established that the *next* one cannot.

**It is a candidate cause for symptoms we have chased elsewhere.** The card is
the boot medium; a data phase that times out and is recovered is exactly the
shape of problem that produces intermittent, timing-dependent failures much
later, and this port has spent real time on those.

**It may be ours rather than upstream's.** The BCM2708 backend under
`soc/sdcard/` is this port's, and it diverges substantially from the arm-native
original it came from — 148 changed lines in `sdcard_bcm2708init.c` alone. The
timeout could as easily be a clock or a wait in our half as a fault in the
generic layer.

# What is left

1. **Find which transfer times out.** It is the first data command after
   sizing, which narrows it to a small window. Naming the command is most of the
   work.
2. **Decide whether it is ours.** Diff `soc/sdcard/sdcard_bcm2708bus.c` and
   `sdcard_bcm2708init.c` against
   `arch/arm-native/soc/broadcom/2708/sdcard/` and look specifically at the
   clock setup and any wait or timeout constant, since a data timeout on a
   controller that is otherwise working points at timing before it points at
   protocol.
3. **Check whether QEMU reproduces it.** It does not appear in QEMU boots, which
   suggests real timing rather than a logic error — worth confirming rather than
   assuming, since QEMU's Arasan model is not the silicon.

# Notes

**Found in a Bluetooth log, which is why it is recorded separately.** It appeared
in every boot of the BT bring-up work on 2026-08-16 and was ignored throughout
because the boot completed. Recording it so it is not rediscovered a third time.

**Do not conflate it with the TLSF corruption.** That is
[`ISSUE-0027`](ISSUE-0027.md), a different message from a different subsystem in
the same logs.

# Execution log

- 2026-08-16 — Opened. Observed on three consecutive boots on a Pi 3 Model B Rev
  1.2 with a 29818 MB card; the same four lines each time, always between the
  capacity report and DOS starting, always recovered.
