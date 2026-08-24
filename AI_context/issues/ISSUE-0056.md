---
id: ISSUE-0056
title: "What the USB interrupt and SOF model should be on this port"
status: open
priority: medium
type: research
owner: unassigned
created_at: 2026-08-24
updated_at: 2026-08-24
tags:
  - usb
  - interrupts
  - sof
  - performance
  - m68k-emu68
blockers:
related_files:
  - aros/arch/m68k-emu68/soc/usb/usb2otg/usb2otg_intr.c
  - aros/arch/m68k-emu68/soc/usb/usb2otg/usb2otg_schedule.c
  - aros/arch/m68k-emu68/soc/usb/dwc2emu68/dwc2emu68_transfer.c
  - AI_context/issues/ISSUE-0047.md
---

# Summary

The two USB drivers this port carries answer the same question in opposite
ways, and neither answer has been measured here. The question is what a host
controller should do about SOF -- the 1 kHz USB frame marker -- and about
interrupts generally, on a machine where every guest interrupt is an m68k
exception delivered by Emu68 as IPL 6.

This is a research issue. Nothing here is a defect; both drivers work.

# The two models

**`usb2otg`: SOF interrupt always on.** The controller raises an interrupt
every frame and the handler decides whether anything is due.
`patches/aros/0022` -- restored 2026-08-24 after being lost in the copy --
narrows what follows: `int_scheduled` gates the `Cause()` so the software
interrupt only runs when a transfer is actually due. The **hardware**
interrupt still arrives 1000 times a second regardless.

**`dwc2emu68`: SOF enabled by event.** `update_sof_irq()` turns the SOF mask
on only while a periodic transfer is queued and off again when the queue
drains. When nothing is polling, no SOF interrupts arrive at all.

ISSUE-0047 records the permanent 1 kHz cost as one of the reasons the rewrite
was started: "Repeated fixes did not remove the permanent 1 kHz SOF interrupt
cost."

# Why this port makes the question sharper

On bare-metal ARM a SOF interrupt is cheap. Here it is not obviously so. Every
guest interrupt goes ARM peripheral -> Emu68 stores level 6 into INTF.IPL ->
m68k level 6 -> `Platform_Autovector()` -> `Dispatch()` -> `krnRunIRQHandlers()`
(see CLAUDE.md, "Boot chain and the IRQ bridge"). That is a JIT-visible
exception path taken a thousand times a second, in a system whose SD driver
already spends 7 ms per command and whose desktop takes a minute to draw.

Whether that matters is exactly what has not been measured.

# What is not known

- What a 1 kHz SOF interrupt actually costs here, in CPU time and in
  interference with the rest of the system. `ISSUE-0042` asks the same about
  the USB stack as a whole and is still open.
- Whether event-driven SOF is even correct for the full case: `dwc2emu68`
  polls interrupt-IN endpoints round-robin at 10 ms without SOF, which is
  fine for one mouse and untested with a hub, several devices, and isochronous
  traffic it does not implement.
- Why `dwc2emu68` needed **8 watchdog recoveries** during one enumeration, all
  on control data stages that completed without raising a channel interrupt.
  Nothing failed, but a transfer engine leaning on its watchdog eight times in
  one enumeration may be missing interrupts that the SOF path would otherwise
  have covered. That observation is the most concrete thread here.

# How it could be measured, without hardware

Both drivers boot under QEMU and one enumerates, so a comparison is possible
now:

1. count interrupts per second in each driver during an idle desktop and
   during enumeration -- a counter in `Dispatch()` or in each driver's top
   half is enough;
2. compare boot-to-`hold released` with USB enabled and disabled, per driver,
   which is already routine here;
3. instrument the `dwc2emu68` watchdog path to say which channel interrupt was
   expected and did not arrive.

QEMU distorts magnitudes, so only ratios and counts are worth anything -- the
standing rule for this port.

# Why it is worth settling

ISSUE-0047 has to choose one of the two drivers, and interrupt cost is one of
the stated reasons the rewrite exists. Choosing on an unmeasured premise would
repeat what ISSUE-0051 records at length: hours spent on a hypothesis that a
single measurement dismissed.
