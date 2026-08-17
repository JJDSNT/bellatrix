---
id: ISSUE-0028
title: "Bluetooth to a usable desktop: what legacy had, what aros-bluzing lacks, and a Zune app for scan/connect/disconnect"
status: backlog
priority: high
type: feature
owner: unassigned
created_at: 2026-08-16
updated_at: 2026-08-16
tags:
  - bluetooth
  - aros-bluzing
  - zune
  - hid
  - legacy
blockers:
related_files:
  - external/aros-bluzing/
  - aros/arch/m68k-native/soc/bluetooth/btuart_init.c
  - AI_context/issues/ISSUE-0019.md
---

# Summary

The controller works. On a Pi 3, `aros-bluzing` reaches
`BT_CONTROLLER_STATE_READY` (`HCI 7.0, mfr 0x000f`, ACL MTU 1021), starts a
passive LE scan, and receives advertising reports — with no firmware patchram at
all.

What is missing is everything between "the radio answers" and "a person pairs a
keyboard": bonding that survives a reboot, HID reports reaching `input.device`,
a connect/disconnect flow, and a way to drive it that is not a boot-time
self-test.

The legacy tree had most of that. It is worth mining for **design and hard-won
operational detail**, not for code: it was built on btstack, and `aros-bluzing`
is a from-scratch stack we own.

# The rule for this issue

**No btstack code is copied.** `external/btstack` in the legacy tree is
third-party and its licence and shape are not what this project wants to carry.
What is ours, and what this issue is allowed to reuse, is
`~/bellatrix-legacy/src/io/bluetooth/` — roughly 3600 lines of *our* glue around
it — and, more valuably, what that glue learned.

# What the legacy had

| File | Lines | What it is |
|---|---|---|
| `bt_host.c` | 58.6K | the host: btstack event loop, connection lifecycle |
| `bt_hal_raspi3.c` | 22.3K | Pi 3 HAL: PL011 open, baud change, flow control, trace ring |
| `bt_scan.c` | 17.3K | inquiry/scan with generation counters, stall detection, recovery |
| `bt_hid.c` | 7.4K | keyboard, mouse and joystick reports |
| `bt_pairs.c` | 6.4K | pairing records |
| `bt_link_key_db_sd.c` | 5.8K | link keys persisted to the SD card |
| `bt_session.c` | 2.1K | "control surface for consumers that do not own the stack" |

**`bt_firmware_stub.c` is one line of evidence worth keeping:**

```c
const uint8_t brcm_patchram_buf[] = {};
const int brcm_patch_ram_length = 0;
```

The legacy also ran with an empty patchram. Two independent implementations have
now reached a working controller on this board without firmware, which settles a
question `protocols/vendor_init/broadcom/README.md` still poses as open.

# What `aros-bluzing` already has

More than its READMEs suggest: `protocols/` carries `hci`, `l2cap`, `att`,
`gatt`; `core/` carries `controller`, `manager`, `device`, `event`, `security`,
`timer`, `hid`. Discovery lands in a unified `bt_device_registry` shared by
Classic inquiry and LE scan, with duplicate filtering.

So the protocol work is largely done. The gap is integration and lifecycle.

# What is left

Ordered so each step is demonstrable on its own.

1. **Connect and disconnect.** `bt_controller_start_le_scan()` and
   `bt_controller_start_classic_inquiry()` exist; there is no equivalent for
   establishing a link. This is the first thing without which nothing else can
   be tried.
2. **Bonding that survives a reboot.** `core/security` exists; nothing persists
   its output. Legacy's `bt_link_key_db_sd.c` is the shape to imitate — link keys
   in a file on the boot volume — and the code is ours to lift if it fits.
3. **HID to `input.device`.** `core/hid` parses reports; nothing feeds AROS. This
   is where "Bluetooth works" becomes "my keyboard types". Legacy's `bt_hid.c`
   did keyboard, mouse and joystick, and its report handling is reusable.
4. **A Zune application: scan, connect, disconnect.** The deliverable the user
   asked for. It needs 1-3 underneath, but its *interface* can be specified now
   and is the thing that makes the rest testable by a person rather than by a
   serial log.
5. **Receive interrupt in `btuart.resource`.** `BTUART_CAP_RX_INTERRUPT` is
   declared in the ABI and never set. Scanning currently works only because the
   manager polls at 1 ms (`ISSUE-0019`); ACL traffic from a connected device will
   not tolerate that. This is a prerequisite for 3 in practice even though it is
   not one on paper.

# Design notes carried from legacy

**Scanning needs a liveness story, not just a start call.** `bt_scan.c` carried
`bt_scan_generation()`, `bt_scan_stall_count()` and `bt_scan_notify_recovery()`
— a scan that stops producing results without erroring is the normal failure,
and it was worth detecting explicitly. `aros-bluzing` has no equivalent, and our
own self-test already hit the symptom: identical traffic, one device found on
one boot and none on the next.

**A control surface separate from the stack owner.** `bt_session.h` describes
itself as "Bluetooth control surface for consumers that do not own the stack".
That is exactly what a Zune application needs, and designing it before writing
the GUI avoids the GUI reaching into the manager task's internals — which our
self-test currently does, taking `Forbid()` around registry reads.

**The UART HAL kept a trace ring.** `bt_hal_raspi3.c` recorded open, open-failed
and set-baud events in a ring buffer readable after the fact. Cheaper than
printing, and it survives the case where the console is the thing that is broken.

# Acceptance criteria

- [ ] A device can be connected and disconnected from software
- [ ] A bonded device is still bonded after a reboot
- [ ] A Bluetooth keyboard produces characters in a Shell
- [ ] A Zune application lists scan results and drives connect/disconnect
- [ ] No btstack source is present in `aros-bluzing`

# Notes

**Order matters more than usual here.** The Zune application is the visible
deliverable, and it is the last thing to build: written first it would be a
window listing devices with two buttons that cannot do anything, and every
defect underneath would present as a GUI bug.

**The self-test is the wrong long-term driver and the right short-term one.**
It runs from `S:User-Startup` and reports over serial, which is what made the
last six defects findable. It should stay until the Zune application can do the
same job, and the application should be judged by whether it can replace it.

# Execution log

- 2026-08-16 — Opened after the controller reached READY and scanned on real
  hardware. Inventoried `~/bellatrix-legacy/src/io/bluetooth/` (3600 lines of our
  own glue over btstack) against `aros-bluzing`'s protocol tree, and found the
  gap is integration and lifecycle rather than protocol. Recorded that legacy
  also ran with an empty patchram, which corroborates today's hardware result
  and answers a question the Broadcom vendor-init README still leaves open.
