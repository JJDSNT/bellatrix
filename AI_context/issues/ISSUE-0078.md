---
id: ISSUE-0078
title: "dwc2emu68: a control SETUP completes and its data stage is never armed"
status: open
priority: high
type: defect
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-30
tags:
  - usb
  - dwc2emu68
  - raspberry-pi-3
blockers: []
related_files:
  - aros/arch/m68k-emu68/soc/usb/dwc2emu68/
---

# Symptom

With `dwc2emu68` on the card the boot stops about eight seconds in and never
reaches the desktop. The last USB line is the SETUP stage of a two-byte
control transfer to the device behind the hub, and it succeeded:

```text
[DWC2/Emu68:XFER] submit #70 chan=0 cmd=12 addr=3 ep=0 len=2 interval=0
[DWC2/Emu68:XFER] arm   chan=0 stage=1 ...
[DWC2/Emu68:XFER] irq   #71 chan=0 stage=1 HCINT=00000023 ...
```

The m68k is alive throughout -- the chipset core samples its PC and finds
ordinary Exec code (`Exec_47_AddTask`, `Exec_61_PutMsg`) -- so this is one
blocked path, not a stopped machine. `AddUSBHardware` runs synchronously from
the Startup-Sequence, so an enumeration that never returns is a boot that
never continues.

# What is already ruled out

- **Not an interrupt storm.** That was the failure before `ded8c79`, and it
  looked completely different: the PC sat in `emu68_DispatchFrame`,
  `scan_bank` and `intc_read`. It is gone.
- **Not the hardware and not the machine.** `usb2otg` on the same board boots
  and drives the mouse. That swap is what localised this.
- **Not the SETUP itself.** `HCINT=0x23` is XFERCOMP+CHHLTD+ACK.

# Where to look

`channel_irq()`'s success path takes `stage == STAGE_SETUP` with
`iouh_Length != 0` to `arm_control_data()`, and `finish(UHIOERR_HOSTERROR)` if
that returns FALSE. Neither outcome appears: no data stage is armed and no
error is replied. Either the call is not reached or it does not return.

**Read the trace with care.** The per-account budget prints the first forty
lines and then one in every 256 (`ded8c79`, `DWC2_TRANSFER_LOG_EVERY`), and
enumeration is only tens of transfers -- so a missing `arm stage=2` line this
late in an account is not evidence that it did not happen. That confusion has
cost four investigations in one day; before concluding anything from a silence
here, drop the interval or add a line that cannot be suppressed.

# What usb2otg does differently

Its handler clears every latched core interrupt it does not dispatch on, which
is what `ded8c79` copied. The rest of the engine differs in shape -- it is
schedule- and SOF-driven where this one defers to a unit task and stages per
channel -- so "do what it does" is not a diff, it is a design to compare
against on the specific path above.
