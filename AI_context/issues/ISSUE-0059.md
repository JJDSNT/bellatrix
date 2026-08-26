---
id: ISSUE-0059
title: "A multi-channel Bluetooth HID device does not come back when its channel is selected again"
status: open
priority: medium
type: investigation
owner: unassigned
created_at: 2026-08-26
updated_at: 2026-08-26
tags:
  - bluetooth
  - bthid
  - hid
  - reconnection
  - raspberry-pi-3
blockers:
  - "a capture from the hardware: which device, which bearer, and whether it fails only after a reboot"
related_files:
  - external/aros/rom/bluetooth/bluetooth/hwconn.c
  - external/aros/rom/bluetooth/bluetooth/config.c
  - external/aros/rom/bluetooth/classes/bthid/bthid.class.c
  - external/aros/rom/bluetooth/stack/protocols/l2cap/channel_manager.c
  - AI_context/issues/ISSUE-0046.md
  - AI_context/issues/ISSUE-0058.md
---

# Summary

Reported from the Pi 3: a Bluetooth HID device with several host channels — the
kind of keyboard or mouse that remembers three hosts and switches between them
with a button — does not come back when its channel is selected again. It was
paired and working on that channel before.

Nothing here is confirmed as a defect in our code yet. This issue exists to
record what the reconnection machinery actually is, which parts are already
written for exactly this case, and the one gap that reading it turned up, so
that the investigation starts from evidence rather than from a re-read.

**Not yet known, and the first thing to establish:** whether it fails only
after a reboot, or also within a single session.

# The three things that have to happen, and where they live

Reconnection is not a boot-time-only path, which is easy to assume from the
commit message of `e1e89712f7` ("restore registered devices ... at boot"). Two
paths are live at all times, and which one applies depends on the bearer:

- **LE** — a background scan reconnects a bonded device when it advertises;
  `bBgScanSchedule()` at `hwconn.c:520`. The stack waits for the advert rather
  than paging.
- **BR/EDR** — page scan stays on. `bgc_Connectable` defaults to `TRUE`
  (`bluetooth.library.c:142`), `btEnumerateHardware()` applies the default scan
  modes at `objects.c:207`, and `BTPRI_SETSCANMODE` (`hwtask.c:1721`) turns that
  into `Write_Scan_Enable` with bit 1 set. The device pages us.

Then, on the incoming connection:

1. `HC_EVT_CONN_REQUEST` (`hwconn.c:2262`) accepts only if the device is
   `BDFF_REGISTERED`, or `bpc_Trusted`, or the radio is discoverable. Anything
   else is rejected with `0x0f` (unacceptable BD_ADDR). **Bonded is not the
   same as registered here**, and that distinction is worth checking first.
2. On connection-complete, `hwconn.c:533-547` walks `bd->bd_Services` and
   registers an L2CAP listener for every endpoint's PSM, "so the device can
   open channels to us". This is direction-agnostic.
3. The device opens the HID PSMs (0x11 control, 0x13 interrupt) and bthid picks
   them up through `btFindEndpoint(..., BEA_PSM, ...)`
   (`bthid.class.c:777`, `:1075`).

# What is already written for this exact case

The design anticipates the channel switch, in both the old and the new bthid,
with the same words:

```c
case BCM_DeviceDisconnected:
    /* the binding stays; its read channels fail and are re-issued when
       the device reconnects (BCHA_AutoConnect) */
```

(`bthid.class.c:496` at pin `fbea2d8`, `:276` at `1986301`.)

The binding surviving the disconnect is what keeps the service alive:
`bConnCleanup()` (`hwconn.c:1187`) frees the services on the bearer **except**
those with a binding — "keep bound services (their binding owns channels)". So
within one session the service list is still populated when the device returns,
step 2 above finds the endpoints, and the listeners go back on.

So within a session this should work. If it does not, it is a defect, and the
place to instrument is the sequence 1 → 2 → 3 above.

# The gap that reading it turned up

**Services and endpoints are never persisted — only device registrations and
bond keys are.** `config.c` writes radio (`BHWD`) and class (`BCLS`) entries and
the device records; it touches `bd_Services` in exactly one place
(`config.c:754`) and that is for class bindings, not to save the service list.
Services come from SDP/GATT discovery, which happens after connecting.

Consequences after a reboot, for a device restored from `ENVARC:`:

- the device pages us and the ACL is accepted (it is registered), but
- `hwconn.c:535` walks an empty `bd_Services`, so no listener is registered, and
- the device's L2CAP connect on PSM 0x11/0x13 lands on
  `channel_manager.c:252`: `/* nobody listening (or no slot): refuse */`.

The symptom that produces is *connects, no input at all* — which is not the
same failure as never connecting, and telling the two apart on the hardware is
most of the diagnosis.

This does not explain a failure within a single session. It is a real gap
either way.

# What to capture

1. **Does it fail only after a reboot, or also on a channel switch within one
   session?** This separates the persistence gap from everything else.
2. **Which bearer — LE or BR/EDR?** Entirely different paths (advert vs page).
   A multi-channel keyboard is often Classic on one channel and LE on another,
   which would also explain "works on one channel, not the other".
3. **Is the device `registered`, or only `bonded`?** `BTDevLister` and the
   device window in Prefs/Bluetooth both show it. Only the first is accepted by
   `hwconn.c:2262` when the radio is not discoverable.
4. Whether the ACL comes up at all, or whether it is the HID channels that are
   refused afterwards.

# Which side of the pin the observation came from

The card this was seen on is at pin `1986301`, i.e. the **old** bthid — the
602-line class that received `bt_aros_input_event` from btcore and emitted only
`IECLASS_RAWKEY` and `IECLASS_RAWMOUSE`.

At `fbea2d8` (ISSUE-0058) the class was replaced by a port of Poseidon's
`hid.class`, which parses report descriptors itself and no longer links btcore.
The reconnection machinery above is in `bluetooth.library` and btcore and is
unchanged by that swap, but **any measurement has to say which bthid it came
from**, because the code that turns a report into an input event is not the
same on the two sides.

# Related

- [ISSUE-0046](ISSUE-0046.md) — the Pi 3 UART controller against the native
  stack; this is downstream of it.
- [ISSUE-0058](ISSUE-0058.md) — the pin bumps and what they landed on.
