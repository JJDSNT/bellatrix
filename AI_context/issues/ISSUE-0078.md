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


# Resuming this: everything you need

## The card is on the other driver

`aros/arch/m68k-emu68/mmakefile.src` now reads

```
#MM- kernel-usb-m68k-emu68 : kernel-usb-nopci kernel-usb-arosotg
```

Switching back is that one word: `kernel-usb-dwc2emu68`. Both are built
either way; only one may be on the card, because the Startup-Sequence has an
`If EXISTS` block per driver and would register both. A card built while this
issue is open carries `Devs/USBHardware/usb2otg.device` and must not also
carry `dwc2emu68.device`.

To test ours without rebuilding a card: `make -C out/build/aros
kernel-usb-dwc2emu68`, then swap the two files in `Devs/USBHardware/`.

## What was fixed on the way here, and must not be undone

| commit | defect |
|---|---|
| `40b80e8` | NAK retry re-armed a channel the core had not released (NAK arrives as `HCINT=0x10` *without* CHHLTD, so the channel is still enabled); the BCM2837 answered XACTERR |
| `4b4f580` | the trace budget was per address, so enumeration on endpoint 0 spent the interrupt endpoint's lines too |
| `1492196` | the unit task ran at priority 0, below Poseidon's own subtasks at `pgc_SubTaskPri = 5`; a 10 ms endpoint was serviced 0 to 37 ms late |
| `013fcc4` | the interrupt-data log stopped at 16 per address and could not tell a silent pipe from a full log |
| `8bb44de`, `9b180f9` | SOF was unmasked at controller init and never masked; `set_sof_irq()` ignored its own argument |
| `ded8c79` | the handler never cleared the latched core interrupts it does not dispatch, storming the m68k until no task was scheduled again |
| `25a5d9f` | the transfer trace ended at 40 lines per account instead of thinning to a heartbeat |

## How to read the trace without being fooled

Three counters exist and they are not equivalent:

- `transfer_log[]` / `transfer_log_ep[]` -- per address, endpoint 0 and the
  data endpoints separately. First 40 lines each, then one in **256**
  (`DWC2_TRANSFER_LOG_EVERY`). **Enumeration is only tens of transfers, so a
  missing `arm` or `irq` line late in an account proves nothing.** Drop the
  interval to 8 or 16 before reasoning about a gap here.
- `interrupt_log_seen[]` -- every interrupt-IN completion; first 16 per
  address then one in 512. This one is safe to reason from.
- `periodic_log_count`, `sof_log_count`, `watchdog_recoveries` -- global,
  first 8. Zero across a whole boot really does mean never.

Four investigations in one day ended in the wrong place because a budget's
silence was read as a device's silence. It is the single most expensive trap
in this driver.

## The observer that survives a stopped m68k

`src/amiga/bus.c`, in `amiga_clock_run_on_core()`, prints once a second from
the chipset core:

```text
[BELLATRIX:LIVE] pc=XXXXXXXX sr=XXXX arm=N ipl=N
```

The kernel ELF is relocatable and loads at **`0x30600000`**, so
`pc - 0x30600000` is an offset into `out/aros/aros-emu68-m68k.elf`. Resolve it
with the tree's own toolchain:

```
out/build/aros/bin/linux-x86_64/tools/crosstools/m68k-aros-nm -n     out/aros/aros-emu68-m68k.elf
```

That is what turned this from three rejected guesses into a fact:
`emu68_DispatchFrame`, `scan_bank`, `intc_read` meant an interrupt storm;
ordinary Exec symbols mean the machine is running and one path is blocked.
`sr` alone says supervisor and IPL and nothing about where -- do not reason
from it.

`aros/arch/m68k-emu68/platform/bcm283x/interrupt_controller.c` also counts
dispatches per IRQ and shouts on the powers of two from 4096:
`[intc] irq N dispatched M times`. Silence there means no storm.

## The state to come back to

The boot reaches `submit #70 chan=0 cmd=12 addr=3 ep=0 len=2`, its SETUP
completes with `HCINT=0x23`, and nothing follows. `channel_irq()` should take
`stage == STAGE_SETUP` with `iouh_Length != 0` into `arm_control_data()`, and
`finish(UHIOERR_HOSTERROR)` if that fails. Neither is observed -- but see the
trace warning above before treating that as established. The first move is to
make the arm/irq lines unsuppressable for one boot and settle whether
`arm_control_data()` is reached.
