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

**The legacy tree got all the way there.** Its own record says so
(`AI_context/consolidated/issue_bluetooth.md`, note of 2026-07-16): *"Bluetooth
HID está funcional — scan, pairing e reconexão automática, e input de
teclado/mouse/joystick chegando ao Amiga, todos comprovados."* The work that
closed it is `AI_context/issues/ISSUE-0059.md`. An earlier section of the same
file says the opposite and is explicitly marked historical — read the note, not
the body.

So this is not exploratory work. It is worth mining for **design and hard-won
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

1. **Connect and disconnect, shaped to fit the reconnect state machine.**
   `bt_controller_start_le_scan()` and `bt_controller_start_classic_inquiry()`
   exist; there is no equivalent for establishing a link, and no notion of
   accepting one. Both directions are needed, and *accepting* is the one that
   carries daily use — see below. Designing the outgoing call without the
   inbound path would produce an API that has to be reopened immediately.
2. **Bonding that survives a reboot.** `core/security` exists; nothing persists
   its output. Legacy's `bt_link_key_db_sd.c` is the shape to imitate — link keys
   in a file on the boot volume — and the code is ours to lift if it fits.
3. **HID to `input.device`.** `core/hid` parses reports; nothing feeds AROS. This
   is where "Bluetooth works" becomes "my keyboard types". Legacy's `bt_hid.c`
   did keyboard, mouse and joystick, and its report handling is reusable.
4. **Classify and name what the scan finds.** Ten unnamed addresses is not a
   device list. HID classification from class-of-device and LE UUID 0x1812,
   remote-name-request as its own phase, and dual-mode merge by public address
   — all four are in legacy's `BTScanResult` and none in ours.
5. **A Zune application: scan, connect, disconnect.** The deliverable the user
   asked for. It needs 1-3 underneath, but its *interface* can be specified now
   and is the thing that makes the rest testable by a person rather than by a
   serial log.
6. ~~**Receive interrupt in `btuart.resource`.**~~ Done 2026-08-16: the PL011
   interrupt drains into a ring and RSSI values came back correct on hardware.
   What legacy had and this does not is the observability around it — the
   watermark, the per-tick budget, and telling silence from a drain failure.

# The reconnect state machine, which is the piece most worth taking

`bt_host.c` carries a connection manager with five states, and its default is the
one that matters: **the device initiates**.

```
PASSIVE ──deadline──> CONNECTING ──timeout──> DISCOVERING ──expiry──> BACKOFF
   ^                       |                       |                     |
   └───────────────────────┴───────────────────────┴──── 60 s ───────────┘
```

- **PASSIVE** is where it sits. The host stays connectable
  (`gap_connectable_control(1)`) and accepts inbound HID links
  (`hid_host_accept_connection`), so a keyboard or mouse that is switched on
  reconnects **by itself**, with the host doing nothing. Outgoing reconnection is
  armed only against a deadline, and only when there is something bonded to
  reconnect to (`bt_pairs_count() != 0`).
- **CONNECTING** has a deadline of its own; on expiry it disconnects the pending
  CID and falls through rather than hanging.
- **DISCOVERING** is the recovery path, not the normal one: scan for 30 s, and
  take the first result that is a HID device *and* already bonded
  (`bt_pairs_is_known`). Finding it returns to PASSIVE — it re-arms the passive
  path rather than connecting immediately.
- **BACKOFF** waits 60 s before trying the cycle again, so a device that is
  simply out of the room costs one scan a minute instead of a permanent one.

Two details in there are worth more than the structure:

**Reconnection can be suspended.** `bt_session_suspend_reconnect()` exists so
automatic outgoing reconnects do not fight the user while they are pairing
something new. Any UI that offers "connect" needs this, and it is the kind of
thing that is obvious only after it has bitten someone.

**Giving up is explicit and says so.** `"recovery discovery expired; passive
reconnect remains armed"` — the host stops *trying* without stopping *listening*.
That distinction is the whole design: the expensive half is bounded, the cheap
half runs forever.

`aros-bluzing` has none of this. It has no connect call at all yet (step 1
below), so the state machine is what step 1 should be built to fit rather than
something bolted on afterwards.

# Why our scan finds ten devices and the user has two

A passive LE scan reports every advertiser in range, which in a home is a
dozen phones, televisions and watches. It is not wrong; it answers the wrong
question. The devices that matter are a keyboard and a mouse, both **dual-mode**,
and a bonded Classic HID device does not advertise on LE at all — it is found by
inquiry.

Legacy's `BTScanResult` (`bt_scan.h`) is the shape that answers the right
question, and every field in it is missing from our `bt_discovered_device`:

```c
uint8_t  transport;   /* bitmask; dual-mode with a public address merges to one entry */
uint32_t cod;         /* Classic class of device */
char     name[25];
uint8_t  name_state;  /* 0 = none, 1 = synthesized label, 2 = real name */
bool     hid;         /* HID: CoD peripheral, or LE UUID 0x1812 */
uint16_t appearance;  /* LE appearance AD */
```

And the header states the hardware constraint that dictates the whole design:

> Phases alternate (inquiry ≈5s → remote-name requests → LE scan 5s → …)
> **because the CYW43438 shares one radio between BR/EDR and LE.**

That is not an optimisation. One radio means a scan that only does LE is
structurally unable to see a Classic keyboard, and doing both means alternating
rather than running them together. Remote-name-request is a third phase because
inquiry gives an address and a class, never a name.

# The 1220-byte advertising event, and why legacy turned LE scanning off

`AI_context/consolidated/issue_bt_scan_stability.md` records this and it is the
single most useful thing in the legacy tree:

> **rxq=330/1220 — LE scan causa overflow do FIFO PL011.** BCM43430A1 empacota
> todos os LE adverts em um único evento de até 1220 bytes. **Fix:** LE scan
> desativado. **TODO(LE):** Reabilitar quando FIFO PL011 puder ser drenado por
> interrupção.

So the overrun we chased on 2026-08-16 was known, and legacy's answer was to
**disable LE scanning entirely**, leaving the condition for re-enabling it
written down: drain the PL011 by interrupt. That landed the same day (see
`btuart.resource`), which means the TODO is now met and this is the first time
LE scanning has been viable on this port.

The number matters for sizing. This controller does not emit one advertising
report per event; it batches every advert it heard into a single HCI event of up
to 1220 bytes. Our H4 reassembly buffer is 4100 bytes and copes, but the
receive ring is 2048 — one such event fits and two do not. **Size the ring
against 1220, not against a tick period.**

# Dual-mode devices appear twice, and that is expected

On hardware a keyboard and a mouse produce **four** HID entries: two from LE with
an appearance, two from Classic with a class of device. That is not a merge
failure to be fixed. Legacy wrote the reason down before it could happen, in the
same comment that disabled LE scanning:

> **TODO(LE):** when re-enabling LE scan, note that HID peripherals (keyboard,
> mouse) typically advertise on a *different* BDA than their BR/EDR address, so
> they will appear as separate entries (CL + LE) rather than merging into DM.

Legacy *has* a unifier — `find_or_add()` matches by address and ORs the
transport bits, so a dual-mode device with one public address becomes a single
`DM` entry. It never had two entries to merge, because with LE scanning off each
device was only ever seen once, through inquiry. That is how it worked with
these same devices.

**So merging by address cannot work here**, and the addresses confirm it: the LE
sightings arrive on random static addresses (`ce:…`, `c1:…` — top two bits set),
which is what an unbonded peripheral advertises with. A device that is bonded
uses its identity address, which is why legacy's comment expects public
addresses at all.

What could merge them, in increasing order of honesty:

1. **Nothing, and say so.** Show both, marked by transport. A user who knows
   they own one keyboard can see that `<keyboard>` appears on CL and on LE.
2. **By type and proximity.** One `<keyboard>` on LE and one on Classic, both
   strong, are probably one keyboard. A guess, and wrong in a room with two.
3. **By identity resolution.** The correct answer, and it needs an IRK from
   bonding — which means it cannot happen before the first pairing, and after
   pairing it is no longer needed for the device already paired.

**Connect on Classic for now.** That is what legacy did — `hid_host_connect()`
with `HID_PROTOCOL_MODE_BOOT` — and HID-over-GATT is not wired in
`aros-bluzing` any more than it was in btstack there. Legacy's note calls LE HID
preferable long term, for latency and WiFi coexistence, and lists the profile
support as the blocker. Until that exists, the LE sighting is information and
the Classic one is the connectable half.

# Two firmware quirks that cost sprints

**`HCI_Read_Local_Name` returns 251 of 252 bytes.** The BCM43430A1 firmware
under-delivers that Command Complete by one byte and the initialisation hangs
waiting for it. Legacy's fix was a btstack option (`ENABLE_AIROC_DOWNLOAD_MODE`)
and is not portable, but the fact is: **do not call Read_Local_Name on this
controller** without deciding what a short Command Complete should do. Our
`bt_controller_on_event()` drops a short event in silence, so this would present
as a bring-up that stops with no message at all.

**Timers fire during re-initialisation.** `bt_scan_notify_recovery()` exists to
cancel in-flight timers and park the phase machine before the stack is
re-initialised, because otherwise HCI commands are issued at a controller that
is being reset.

# Bonding storage: a format, already designed

`BTPAIRS.TXT`, one line per bonded device:

```
CL K AA:BB:CC:DD:EE:FF nome\r\n
 │  └─ type: K keyboard, M mouse, J joystick, ? unknown
 └──── transport: CL classic, LE, DM dual-mode
```

Plain text, parsed and serialised without libc. Two details worth keeping: the
type and transport are stored rather than re-derived, so a bonded device is
classified even before it is seen again; and the file was created by the build
as a fixed 512-byte placeholder because overwrite-in-place needs it to pre-exist
— a constraint of that bare-metal FAT writer that AROS does not have, but the
format survives it.

# Connection flow, as it actually worked

1. `l2cap_init`, `sdp_init`, `hid_host_init`, a link-key database, then register
   the HID packet handler.
2. On reaching the working state, walk the bonded list and connect each with
   **`HID_PROTOCOL_MODE_BOOT`** — boot protocol, not report protocol.
3. `CONNECTION_OPENED` records cid → device type in a table, looked up by
   address against the bonded list. `REPORT` dispatches on that type, **with a
   fallback on report size when the type is unknown**. `CONNECTION_CLOSED`
   releases every key held by that cid, so a disconnect cannot leave a key stuck
   down.

# The scan UI, already specified

Worth copying into the Zune application rather than reinventing: `>` cursor,
`[P]` for paired, `[*]` for HID; UP/DOWN move, ENTER toggles pairing, DEL removes
and saves, ESC leaves. The two markers are the whole reason the list is readable
— ten addresses become "the two that matter, one of them already known".

# Operational detail from ISSUE-0059, worth more than the code

Ordered by how much trouble it saves.

**Silence is distinguished from a drain failure.** A non-empty PL011 FIFO when
nothing is arriving indicts the IRQ or host path, and suppresses the
power-cycle that would otherwise mask it. We now have the interrupt that makes
this checkable and no such check.

**The IRQ top half preserves FE/PE/BE/OE from every read**, and the ring
watermark is published. We do the first half (see `BTUARTRead`) and not the
second.

**Receive has a per-tick budget** — bytes and H4 callbacks both — so a burst
cannot starve the reactor, with counters logged.

**Recovery advances in four ticks** (`request off`, `force initializing`,
`force off`, `physical reset/PatchRAM`) rather than one monolithic power cycle,
because a reentrant HCI transition inside the reactor was the failure being
avoided. Anything received inside a stack callback only *publishes* intent; the
transport is mutated later, from the reactor.

**Link keys are kept on ordinary failure.** Only `AUTHENTICATION_FAILURE` and
`PIN_OR_KEY_MISSING` prove a key was rejected; anything else means the device is
merely absent. Deleting a good key because a device was out of the room is the
mistake this prevents.

**Outbound connects one pair at a time, mouse first**, because the embedded SDP
client has a single context. Inbound is always accepted and the host stays
connectable.

**HID details that cost debugging time:** the HIDP `0xA1` DATA/Input prefix is
stripped before the decoder; descriptor, `SET_PROTOCOL` and `GET_PROTOCOL`
events update session state instead of falling through as unknown; error reports
`0x01..0x03` are ignored on both USB and Bluetooth; a Classic mouse with Report
ID 1 and signed 16-bit little-endian deltas is decoded without treating other
report IDs as movement; and after connecting, `EXIT_SUSPEND` and `GET_PROTOCOL`
go out in *separate* reactor steps.

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
- [ ] A bonded device switched on reconnects on its own, with no software action
- [ ] Automatic reconnection can be suspended while the user pairs something
- [ ] The receive ring is sized against a 1220-byte advertising event
- [ ] The scan lists HID devices by name, not every advertiser by address
- [ ] A dual-mode keyboard is found by inquiry, not only when it advertises
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
