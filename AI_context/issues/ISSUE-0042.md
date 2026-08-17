---
id: ISSUE-0042
title: "What the USB stack costs the rest of the system, now that interrupts have a guest-side price"
status: backlog
priority: high
type: investigation
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - usb
  - irq
  - performance
  - emu68
  - measurement
blockers:
related_files:
  - aros/arch/m68k-emu68/platform/platform.c
  - aros/arch/m68k-emu68/platform/bcm283x/interrupt_controller.c
  - patches/emu68/0010-assert-host-interrupts-on-intf-arm-with-a-guest-owned-ack.patch
  - patches/aros/0022-usb2otg-only-schedule-work-for-due-sof-transfers.patch
  - AI_context/consolidated/history/ISSUE-0039.md
  - AI_context/issues/ISSUE-0041.md
---

# Summary

Not "is USB fast". **How much does having USB running cost everything else.**

The stack raises interrupts continuously — the inherited DWC2 driver is driven
by a 1 kHz SOF — and on this port every one of those is a full m68k interrupt
round trip through JITted code. That is a tax paid by the desktop, by the card,
and by anything a user runs, whether or not they are using USB at that moment.

Two things make this worth opening now rather than later.

# 1. Every performance number this project has was taken with USB idle

Checked, not assumed. Across the seven instrumented boots recorded on
2026-08-17, every serial log contains

```
[USB2OTG] Init: No device connected on port!
```

and none contains a `Device connected` line. `scripts/boot-timing.py` — the
tool behind the 153 runs in `out/boot-timing.jsonl`, including the 44.5 s
median to icons — **never attaches a USB device at all**; the string "usb" does
not appear in it. `run.sh` adds `-device usb-tablet` only for graphical runs.

So the whole performance record of this project describes a machine whose USB
controller is initialised and then has nothing to do. Those numbers are a
**floor**, not a picture, and the gap between them and a machine with a mouse
and a keyboard plugged in has never been measured.

# 2. Interrupts stopped being free on the guest side today

Until 2026-08-17 the platform IRQ path wrote a level into `INTF.IPL` and the
arbitration dropped it as it took the exception. There was **nothing for the
guest to do**: no acknowledge, no register access, no instructions.

ISSUE-0039 moved platform interrupts to `INTF.ARM`, which is a latch. The guest
now deasserts it, and while the mechanism is as cheap as it can be — a `MOVEC`
read-modify-write on JITCTRL2 bit 29, three register instructions, no memory
access and no fault — it is **not zero, and it is paid per interrupt**.

That change was right and is not in question here (it is what the design record
asks for, and it frees `INTF.IPL` for Rigel). What it does is turn "how many
interrupts per second does USB generate" from an idle curiosity into a number
with a coefficient attached.

# What one interrupt actually costs here

The chain, from `CLAUDE.md` and `platform.c`, with the parts that are JITted
m68k marked:

```
ARM peripheral
  -> Emu68 core-0 vector: strb #1 -> INTF.ARM          (AArch64, ~3 instructions)
  -> ExecutionLoop arbitration: INTF.ARM -> level 6, tested against SR
  -> m68k exception, autovector 6
  -> Platform_Autovector_Direct: movem.l save           [JIT]
  -> platform_host_irq_ack(): MOVEC read/modify/write   [JIT]
  -> g_intc_ops->Dispatch(): poll the ARM controller's
     pending registers to find which source fired       [JIT]
  -> krnRunIRQHandlers()                                [JIT]
  -> usb2otg handler                                    [JIT]
  -> RTE, JIT reopens the ARM IRQ gate when SR drops
```

Two properties of this that matter for a 1 kHz source:

* **Everything after the exception is translated m68k.** An interrupt that
  costs a few hundred cycles on `arm-native` costs considerably more here, so
  the inherited driver's assumptions about interrupt cost do not carry over.
  This is the same reasoning that made `CPUSHP` beat a `CPUSHL` line loop by
  eleven seconds of boot in `CacheClearE`: under a JIT the unit of cost is the
  instruction.
* **One line, all sources.** Disk, DMA, USB and Bluetooth all arrive as level 6
  and `Dispatch()` polls the controller to find out who. Adding sources
  lengthens dispatch **for every source**, so USB's SOF rate is not only USB's
  problem.

# What is already known and what it implies

* `patches/aros/0022` stopped the driver signalling `PendingInt` on every SOF,
  removing up to 1000 needless scheduler wakeups per second. The SOF interrupt
  itself still fires at 1 kHz — the patch removed the downstream work, not the
  interrupt.
* ISSUE-0019's `denebusb` analysis is the direction: mask SOF while no
  near-term frame-sensitive work needs it, and wake from a task-local timer at
  the earliest interrupt-pipe deadline. That is an optimisation of the *source*
  and belongs in ISSUE-0041's queue.
* `dma.resource` is now a second interrupt consumer when a caller passes
  `DMACHF_IRQ` (ISSUE-0013). The SDHOST backend does not — it calls
  `DMAAllocChannel(0)` and waits by polling — so today only the boot probe
  exercises that path. Worth knowing before someone enables it.

# Bringing the stack up is itself a cost, before any device exists

This is the measurement to take first, and it is not the one about attaching a
device. The whole stack enters from the Startup-Sequence and nowhere else:

```
:37   If EXISTS "SYS:Classes/USB"
:39       Run <NIL: >NIL: QUIET AddUSBClasses
:119  If EXISTS "DEVS:USBHardware/usb2otg.device"
:121      AddUSBHardware DEVS:USBHardware/usb2otg.device >>"...usb-hardware.log"
```

So a card copy with those two blocks removed is an exact control: same ELF,
same everything, minus the stack. What that measures, all of it before a device
is ever plugged in:

* **`AddUSBHardware` is synchronous.** Line 121 blocks the Startup-Sequence, so
  device Init, `OpenUnit`, the core soft reset and the settle delays are
  directly on the path to the desktop, not in the background.
* **`AddUSBClasses` loads every class under `SYS:Classes/USB`.** That is card
  I/O, on a boot that reads about 7 MiB in total (ISSUE-0013) at a rate this
  project has only just started measuring.
* **Whatever keeps running afterwards.** The driver creates a `USB2OTG Worker`
  task pinned to CPU0, and Poseidon has tasks of its own. They are scheduled
  whether or not anything is attached.
* **Whether the core still raises SOF with no device.** Not established. The
  port reports `No device connected` and halts its channels, which *suggests*
  the 1 kHz source is quiet — but suggesting is not measuring, and this is
  exactly the question the counter in measurement 4 answers.

Three runs each way, idle host, `boot-timing.py`.

# Then, with a device

1. **Boot with and without `-device usb-tablet`**, three runs each. This is the
   gap in point 1 above: no measurement this project has ever taken had a
   device attached.
2. **Idle-attached versus actively-reporting.** A tablet that is not moving is
   still polled; the difference between those two says how much of the cost is
   enumeration and how much is traffic.
3. **Count level-6 entries.** `Platform_Autovector()` already has a bounded
   trace (`entries < 3`), compiled out behind `PLATFORM_TRACE_BRINGUP`. A free
   running counter reported per second would give interrupts/second directly,
   and would keep working as sources are added.
4. **On hardware.** QEMU distorts magnitude — device-model MMIO is far more
   expensive than the real thing, and the SOF timing is emulated. The *shape*
   is obtainable here; the number that decides is on the Pi.

# Notes

**This is not ISSUE-0041.** That one is about USB not working for whole classes
of device, which is correctness. This one is about what USB costs when it *is*
working, which is everyone else's problem.

**And it is not a reason to undo ISSUE-0039.** The guest-side acknowledge is
three register instructions and the alternative was a channel that cannot be
acknowledged without a chipset. What changed is that the cost is now countable,
which is an improvement over a cost that was hidden in the arbitration.

**Inside the standing freeze**: measurement, and making what exists faster.

# Execution log

- 2026-08-17 — Opened at the user's request, on the observation that the
  question is not USB's own throughput but the stack's effect on system
  performance, and that the IRQ path changed the same day. Two things came out
  of writing it: that every existing measurement was taken with no USB device
  attached, and — the user's point, which reordered the plan — that **merely
  bringing the stack up already costs**, before any device exists. The control
  for that is a card without the two Startup-Sequence blocks, which is cheaper
  and more conclusive than attaching hardware.
