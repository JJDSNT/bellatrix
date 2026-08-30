---
id: ISSUE-0076
title: "A USB mouse enumerates and is never asked for its interrupt pipe"
status: open
priority: high
type: defect
owner: unassigned
created_at: 2026-08-30
updated_at: 2026-08-30
tags:
  - usb
  - dwc2emu68
  - timing
  - raspberry-pi-3
blockers: []
related_files:
  - aros/arch/m68k-emu68/soc/usb/dwc2emu68/
---

# Symptom

A USB mouse enumerates and then never works. On hardware, with the machine
reaching Wanderer:

```text
[DWC2/Emu68:XFER] submit #67 chan=0 cmd=12 addr=3 ep=0 len=0
irq #69 chan=0 stage=3 HCINT=00000010     NAK
irq #70 chan=0 stage=3 HCINT=00000082     XACTERR
irq #71 chan=0 stage=3 HCINT=00000082     XACTERR
[DWC2/Emu68:XFER] error chan=0 stage=3 HCINT=00000082 cmd=12 addr=3 ep=0 len=0 HPRT=00001005
```

Enumeration itself completes: address 3 is assigned and the descriptors are
read at `len=9` and `len=39`, which is a HID sequence. What fails is the status
stage of a zero-length control transfer. The device NAKs -- it is busy -- the
driver re-arms, and the retry gets a transaction error, three times.

# It was always broken; the console was hiding it

The defect appeared the moment `src/amiga/console.c` landed, and the mechanism
is not subtle. `[DWC2/Emu68:XFER]` prints several lines per transaction, and
before the console sink every `kprintf` blocked on the UART for about nine
milliseconds at 115200 baud. **That rate-limited the USB driver, and nobody
asked it to.** With the sink, `kprintf` returns in microseconds, the driver
runs orders of magnitude faster, and the post-NAK retry lost the accidental
delay it had been relying on.

Confirmed by a probe build, `CONFIG_RIGEL_CONSOLE_SINK=0`: the same chipset
core, the console written through as before, and **the mouse works**.

**That probe is not the fix and must not be mistaken for one.** Turning off a
working subsystem so another one's latent defect stays hidden is not a repair;
it is losing the diagnosis and the fix at once. The shipping configuration
keeps the sink.

# Where the fix belongs

A NAK on the status stage means "not yet", and the host controller driver has
to honour an interval before re-arming rather than resubmitting immediately.
The three retries burning through in microseconds is the visible shape of
having no interval at all.

Worth checking while there: the same retry path is used for the data stage, and
`[DWC2/Emu68:WD] recoveries` in a later log shows the watchdog firing on
`stage=4` as well, so this may not be the only place that assumes it is being
slowed down by something else.

# The general lesson, which is the expensive part

An accidental delay that a driver depends on is a defect in the driver, and it
is invisible until something removes the delay. Nothing about the console sink
is wrong; it exposed a bug that had been there since the driver was written.
Expect more of these: **every subsystem that was developed while the console
was writing through has been running with a millisecond-scale delay sprinkled
through its logging paths.**


# 2026-08-30: the NAK is fixed, and it was not the reason the mouse is dead

`40b80e8` stops the channel before re-arming it. A NAK arrives as
`HCINT=0x10` **without** CHHLTD, so the channel is still enabled and the
re-arm was writing CHENA on a channel the core had not released; the BCM2837
answered XACTERR. Confirmed on hardware: `irq #69 HCINT=00000010` is now
followed by `irq #70 HCINT=00000023`, zero transaction errors, and address 3
finishes its enumeration.

The mouse still does not work, and the next reading of the log was wrong in
the same way twice: "after transfer #74 nothing further is submitted to
address 3". It is not. `DWC2_TRANSFER_LOG_LIMIT` is 40 lines **per device
address**, and enumeration spends all forty on endpoint 0 -- so the trace goes
silent exactly where it starts to matter. `4b4f580` gives endpoint 0 and the
data endpoints separate accounts.

What does establish something are the three counters that were never
per-address, and all three are at zero in every log recorded so far:

| counter | what its absence rules out |
|---|---|
| `[DWC2/Emu68:SCHED] SOF #n` (`sof_log_count < 8`) | the periodic queue was never non-empty, so `update_sof_irq()` never enabled the SOF interrupt |
| `[DWC2/Emu68:SCHED] NAK #n` (`periodic_log_count < 8`) | no periodic IN was ever parked, which is what an idle mouse's pipe does continuously |
| `[DWC2/Emu68] interrupt data #n` (`interrupt_log_count[addr] < 16`) | no interrupt transfer ever completed with data |

**No interrupt IN transfer has ever reached the driver.** The failure is not
in the host controller; it is above it, in whatever should be binding a class
to the device and opening its pipe.

# Where that points

`kernel-usb-nopci` -- the metatarget this port uses -- does **not** include
`kernel-usb-usbromstartup`, so the two ROM residents that upstream relies on
never run here. `hub.class` and `hid.class` are not registered at coldstart;
every class comes from `AddUSBClasses` scanning `SYS:Classes/USB`, started
from the Startup-Sequence as

```
Run <NIL: >NIL: QUIET AddUSBClasses
```

asynchronously, silently, and with its output on `NIL:`. `psdAddClass()` does
not rescan: a class registered after the last `psdClassScan()` binds to
nothing already present. The only scans are the one at the end of
`AddUSBClasses`, the one in `AddUSBHardware`, and the one `hub.class` runs
per newly configured device. With the console sink the Startup-Sequence races
ahead of a background command that has to load about a megabyte of class files
from the card, and the ordering between those two is no longer what it was
when the console cost nine milliseconds a line.

That is a hypothesis, not a finding. `S:usb-report` (`9c7932a`) settles it in
one boot: it prints the registered classes, `PsdDevLister`'s topology with its
bindings, and Poseidon's own error log -- `hub.class` writes "New device '%s'
at port %ld" through `psdAddErrorMsg()` for every device it configures, and
every failure on the way there writes a line too.
