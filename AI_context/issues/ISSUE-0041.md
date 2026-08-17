---
id: ISSUE-0041
title: "USB: modern devices hang the port, and the legacy stack failed the opposite way"
status: backlog
priority: high
type: investigation
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - usb
  - usb2otg
  - poseidon
  - dwc2
  - performance
  - legacy
blockers:
related_files:
  - external/aros/arch/arm-native/soc/broadcom/2708/usb/usb2otg/
  - aros/arch/m68k-emu68/soc/usb/usb2otg/mmakefile.src
  - patches/aros/0013-usb2otg-do-not-name-arm-instructions-in-a-portable-dr.patch
  - patches/aros/0022-usb2otg-only-schedule-work-for-due-sof-transfers.patch
  - AI_context/issues/ISSUE-0019.md
---

# Summary

The USB port works: `usb2otg.device` builds for m68k, Poseidon enumerates,
descriptors are read, and HID mouse buttons and motion reach `input.device`
(ISSUE-0019). What it does not do is work for everything.

**It hangs when a more modern device is attached.** And the stack this project
used before the 2026-08-03 reset failed the *opposite* way — that one did not
work with older devices.

Two stacks, two disjoint failure sets, on the same silicon. That is the most
useful fact on this page, and it is why this issue exists separately from
ISSUE-0019, which is about getting USB to exist at all.

# Why the complementary failure is the whole clue

A stack that handles old devices and hangs on new ones, versus a stack that
handles new ones and fails on old ones, is not two random bugs. On this
controller the difference between the two populations has a name.

The Pi 3B's DWC2 root port is **high-speed**, and there is a hub in the way
(the LAN9514 carries hub and Ethernet). So:

* A **high-speed** device talks to the controller directly, at 480 Mbit/s, with
  larger `wMaxPacketSize`, potentially multiple configurations and interfaces,
  and descriptor sets that do not fit one transfer.
* A **low- or full-speed** device behind a high-speed hub cannot. Its traffic
  has to be carried in **split transactions** — SSPLIT/CSPLIT, scheduled
  against microframes — which is an entirely different code path in the host
  controller driver.

So the two stacks probably did not fail at the same layer at all. One likely
got split transactions right and high-speed handling wrong; the other the
reverse. Establishing which is the first job here, and it is cheap: it is a
question about *which device* fails, not about reading 9000 lines.

Note ISSUE-0019 already records that the inherited driver has split-transaction
code — *"split-transaction channels that get retired when they cannot be
revived"* — so the machinery exists and the question is whether it is correct
and whether it is reached.

# What is not known, and has to be before anything else

Nothing below is measured. The symptom is a user report, and this issue starts
by turning it into evidence.

1. **Which devices, exactly?** Make, speed (low/full/high), whether behind a
   hub, whether the hang is at enumeration or later. "Modern" and "older" are
   the observation, not the classification.
2. **What does "hangs" mean?** Does the boot stop, does Poseidon's task spin,
   does the whole machine stop responding? The port has a `USB2OTG Worker` task
   pinned to CPU0; a hang in it and a hang of the machine look the same from
   the outside and are not the same defect.
3. **Where in the sequence?** ISSUE-0019 has traces for reset, OpenUnit,
   address assignment, descriptor reads and interrupt scheduling. The failing
   case should be placed against that sequence before any code is read.

Answering all three is one boot per device with the existing diagnostics.

# The legacy stack: mine it for knowledge, not for code

The previous incarnation of this project drove DWC2 through **CherryUSB**
(`~/bellatrix-legacy/external/cherryusb`, wired in
`legacy/scripts/setup.sh:24,218` with a `0004-bellatrix-cherryusb-dwc2-host`
patch).

**Do not copy code from it.** Two independent reasons:

* **Licence.** CherryUSB is Apache-2.0; AROS is under the AROS Public License.
  Moving source between them is not a thing to do casually, and this port's
  code goes into AROS's tree.
* **It is a different shape.** CherryUSB is a bare-metal host stack with its
  own device model. Our driver is an AROS `usb2otg.device` under Poseidon,
  which owns enumeration, classes and the device list. Grafting parts of one
  into the other produces a third thing that neither upstream maintains.

What the legacy tree is genuinely good for:

* **Which devices worked there**, so the failing set here can be compared
  against a set that is known to have worked somewhere.
* **The DWC2 register sequences it used** — reset, port init, channel setup,
  split scheduling — as a *reference to read* when ours misbehaves. Reading a
  second implementation of the same hardware is how ambiguities in the
  datasheet get resolved.
* **What it got wrong with old devices**, which is a hint about where the
  split-transaction handling in our driver should be scrutinised hardest.

# Optimisation, separately from correctness

ISSUE-0019 already carries the analysis and it should move here as work rather
than be repeated:

* **The SOF scheduler.** The inherited driver promoted queued interrupt work
  from a 1 kHz SOF interrupt. `patches/aros/0022` stopped it causing
  `PendingInt` on every SOF, removing up to 1000 needless wakeups per second.
  The larger change is still open: **mask SOF while no near-term frame-sensitive
  work needs it**, and wake from a task-local timer at the earliest interrupt
  pipe deadline instead.
* **The lesson from `denebusb`** (ISSUE-0019). The Amiga ISP1760 driver
  deliberately leaves SOF disabled and enables only transfer-done, port-change
  and frame-rollover, letting hardware enforce endpoint intervals. DWC2 has no
  PTD scheduler so this cannot be copied register-for-register, but the policy
  transfers: do not pay one emulated IRQ per USB frame to discover that a 10 ms
  mouse deadline is not due.
* **Why it matters more here than on ARM.** Every one of those interrupts is
  delivered to an m68k guest through the level-6 path (ISSUE-0039) and handled
  in JITted code. An interrupt that costs a few hundred cycles on arm-native
  costs considerably more here, so the arm-native driver's assumptions about
  interrupt cost do not carry over.

**Correctness first.** A device that hangs the port is not a performance
problem, and tuning the scheduler underneath a broken transfer path would make
the hang intermittent rather than fix it.

# What to do

1. Turn the symptom into evidence: the three questions above, one boot per
   device.
2. Classify the failing and working devices by speed and by whether they sit
   behind a hub. If the split lands on that boundary, the investigation has its
   target.
3. Only then read code — ours first, the legacy CherryUSB DWC2 path second, as
   a reference.
4. Keep the SOF work queued behind that.

# Notes

**Under the standing freeze this is diagnosis and repair of what exists**, not
new functionality. USB already works on this port; it works for a subset.

**Wider device support is the goal, not a new stack.** Poseidon is the device
model here and stays. What is missing is correct DWC2 handling for the
populations that currently fail.

**Do not let this merge with ISSUE-0019.** That issue is the port: getting
`usb2otg.device` built, attached and enumerating, which is done. This one is
what the port does not yet cover.

# Execution log

- 2026-08-17 — Opened at the user's request. The defining observation is
  theirs: the current port hangs with more modern devices, and the legacy
  CherryUSB-based stack failed the other way round, on older ones. No
  measurement yet — the symptom has not been reduced to a device, a speed, or a
  point in the enumeration sequence.
