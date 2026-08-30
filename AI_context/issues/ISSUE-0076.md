---
id: ISSUE-0076
title: "dwc2emu68 retries a control transfer too soon after a NAK"
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
