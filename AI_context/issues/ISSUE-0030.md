---
id: ISSUE-0030
title: "What the legacy Bluetooth port failed at: remote-name-request worth retrying, H5 probably not"
status: backlog
priority: low
type: feature
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - bluetooth
  - aros-bluzing
  - hci
  - legacy
blockers:
related_files:
  - external/aros-bluzing/protocols/hci/hci.c
  - external/aros-bluzing/core/controller/controller.c
  - AI_context/issues/ISSUE-0028.md
---

# The principle this issue exists to hold

**An old failure is a measurement, not a verdict.** Every finding below was made
by a different port, on a different stack, in a state this one is not in. What
such a finding is good for is naming the thing to measure first; what it is not
good for is deciding in advance that something cannot work.

That is easy to say and easy to violate. It was violated twice while writing
this file: first by reading "Remote Name Request crashes the firmware" as a
property of the chip, then by assuming the opposite -- that the legacy port ran
without firmware -- when its build downloads `BCM43430A1.hcd` and compiles it
in. Both readings were about avoiding the measurement.

# Summary

Two things the legacy port set out to do and did not finish. They are recorded
together because they were abandoned together, and they deserve opposite
answers.

**Remote-name-request: worth retrying, and we are better equipped than it was.**

**H5 transport: probably never needed, and the evidence for that is that
Bluetooth works without it.**

# Remote-name-request

Legacy planned a discovery phase for it — `SCAN_CLASSIC_NAMES`, described in its
own enum as *"remote name requests, one at a time"* — and shipped without the
implementation. All that survives is the enum entry and one status string,
`"resolving names..."`. What went in instead is the ladder we have now: a real
name when the device advertises one, a synthesized `<keyboard>` label otherwise.

**Why it matters.** A label identifies a kind, not a device. With one keyboard
that is enough; with two it is not, and "pair the second `<keyboard>`" is not
something a user can act on. Classic devices that answer inquiry without an
Extended Inquiry Result have no name at all through any other route.

**The legacy port ran this chip with its firmware loaded, and we do not.**
`scripts/build.sh` there downloads `BCM43430A1.hcd` and generates a C source
from it, which CMake compiles in as `BELLATRIX_BTSTACK_PATCHRAM_SOURCE`;
`bt_firmware_stub.c` with its empty array is only the fallback, and the build
warns when it is used. `issue_bluetooth.md` records "PatchRAM visível e correto
no boot".

So its two findings about name commands were made on a **patched** controller:

- `50f5fa9` — *"Remote Name Request crasha o firmware BCM"*
- `rxq=251/252` — `HCI_Read_Local_Name` returns one byte short and hangs

That makes them harder to dismiss, not easier. But it also exposes a difference
between the two ports that has not been accounted for anywhere: **this port has
no patchram at all**, so our controller is running the ROM image while legacy's
was running the patch. Everything measured here — that HCI works, that LE
scanning works, that inquiry works — was measured in a state legacy was never
in. Whether the name commands behave the same in both is unknown and is the
first thing to establish, in either direction.

**Why it is worth retrying rather than accepting.** There is a specific,
testable hypothesis for why it failed, and it is not "remote names are hard".
The same document that records the abandonment records this:

> **rxq=251/252 — `HCI_Read_Local_Name` trava init após recovery.** Firmware
> BCM43430A1 envia 251 de 252 bytes do Command Complete.

That is a name-bearing command whose Command Complete arrives **one byte
short**, and initialisation hangs waiting for the rest. `Remote_Name_Request`
completes with a 248-byte name field in an event of the same shape. If the
firmware truncates it the same way, the failure is identical and has nothing to
do with our code.

**We can see that now and legacy could not.** `deliver_packet()` logs the
reassembled length at the transport boundary, so a short event is visible
immediately rather than as a hang. And it is worth fixing regardless of names:
`bt_controller_on_event()` currently drops a short event **in silence** —
`bt_buf_reader_peek(&r, hdr.param_len)` returns NULL and the function returns —
so any under-delivered event anywhere presents as nothing happening at all.

## What to do

1. **Make a short event say so.** One line in `bt_controller_on_event()`. Cheap,
   independent of names, and it converts a whole class of silent failure into a
   message.
2. **Then try `Remote_Name_Request`** on a device found by inquiry, and read the
   delivered length. If it is short by one, the hypothesis holds and the question
   becomes what to do about a controller that under-delivers — accept the
   truncated name, most likely, since a name is not load-bearing.
3. If it is not short, the failure is elsewhere and legacy's note is a red
   herring; the instrumentation from step 1 still pays for itself.

# H5, and why this recommends against it

Legacy's bring-up plan had three phases: controller reset and settle, HCI reset
over **raw H4**, then **H5 as the main transport**. Phase 1 never got a valid
response during that sprint, so phase 3 was never reached and the plan was
suspended in favour of USB HID.

The plan's premise was never tested, and everything since argues against it:

- **H4 demonstrably works.** This port reaches `BT_CONTROLLER_STATE_READY`,
  scans, and inquires, over H4 with hardware flow control.
- **H5 solves lossiness** — sliding window, retransmission, CRC. It exists for
  links that drop bytes. Ours is a PCB trace with RTS/CTS.
- **Every byte we lost was our own fault.** The FIFO overruns of 2026-08-16 came
  from draining the UART on a task tick; the fix was the receive interrupt, not
  a transport with retransmission. A reliability layer over a link whose only
  unreliability was self-inflicted buys nothing and costs a state machine.
- Linux drives this chip over H4 by default.

**The one case that would change this**: raising the baud rate. Vendor bring-up
normally switches the BCM43438 to 3 Mbaud after firmware download, and errors
that do not exist at 115200 can appear there. If that day comes, measure the
UART error bits — `PL011BTRead` reports them now — before reaching for H5.

# Decisions taken

**Do not implement H5 speculatively.** It is a fix for a problem this port has
not been shown to have, and the port that planned it never established that it
did either.

**Do not treat legacy's abandonment as a verdict.** It stopped for a reason it
recorded — no valid response in phase 1 — that has since been explained and
fixed by other means. What it abandoned is not thereby proved impossible.

# Acceptance criteria

- [ ] A short HCI event is reported rather than dropped in silence
- [ ] `Remote_Name_Request` has been tried once against a real device, and the
      delivered event length recorded either way
- [ ] Either Classic devices without an EIR name get a real name, or the reason
      they cannot is written down with the measurement behind it
- [ ] The name commands have been tried on **this** port, whose controller is
      unpatched, so the result is attributed rather than inherited from a port
      that ran the patch

# Notes

**The names we do get are good enough to be going on with.** On hardware the LE
appearance identified a keyboard (0x03C1) and a mouse (0x03C2) correctly, which
is the case that matters most, and Classic class-of-device distinguishes
keyboard from pointing device. This issue is about the gap, not about a hole.

# Execution log

- 2026-08-17 — Opened while bringing the name ladder across from legacy. The
  ladder itself landed; this records the tier above it that did not, and the
  transport that was planned and never needed.
