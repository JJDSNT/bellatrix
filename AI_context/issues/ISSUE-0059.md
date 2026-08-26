---
id: ISSUE-0059
title: "A multi-channel Bluetooth mouse does not come back when its channel is selected again"
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
  - low-energy
  - raspberry-pi-3
blockers:
  - "two free hardware tests — see 'What to try before writing code'"
related_files:
  - external/aros/rom/bluetooth/bluetooth/hwtask.c
  - external/aros/rom/bluetooth/bluetooth/hwconn.c
  - external/aros/rom/bluetooth/classes/bthid/bthid.class.c
  - AI_context/issues/ISSUE-0061.md
  - AI_context/issues/ISSUE-0046.md
---

# Summary

A Bluetooth mouse with several host channels — the kind that remembers three
hosts and switches with a button — does not come back when its channel is
selected again on a Raspberry Pi 3.

This issue is about **this board and this symptom**. The design gaps in the
stack that the investigation turned up are [ISSUE-0061](ISSUE-0061.md); audio
support, which came up in the same conversation, is
[ISSUE-0060](ISSUE-0060.md). What follows is what we can do here.

# What is already known to work

Measured on hardware, 2026-08-26, after the firmware and Startup-Sequence fixes
of the same day. None of this needs re-testing:

- the radio brings up with the patchram applied inline and a real address:
  `bring-up complete - HCI 7.0 LMP 7.8713, BR/EDR+LE, addr B8:27:EB:34:97:7D`;
- the device is restored from the saved config across a reboot
  (`1 registered device(s) restored`);
- **reconnection after a reboot works** — so persistence, private-address
  resolution and the background scan all function;
- leaving the channel is clean and fully observed:

      Disconnected from <name> (remote user terminated connection).
      Reading input reports failed: link dropped (28).

- returning to the channel produces **no log line at all** — neither
  `is awake - connecting.` nor `- reconnecting.`.

That last pair is the whole finding. Reconnection is not broken; the case where
a live link dropped and the device later returns is.

# The device

From the Bluetooth prefs device window:

- **Low Energy only.** Stored keys are `LE key (legacy)` and `IRK`, with no
  `BR/EDR link key` (the string is built in `prefs/DevWinClass.c:89-91`).
- state: `registered bonded`
- per-device auto-reconnect: **on** (`bpc_AutoConnect`)
- per-device trusted: **off** (`bpc_Trusted`) — and correctly so, see below

# Trusted is not the answer

An earlier revision of this issue put `Trusted` first. That was wrong, and the
reason is worth keeping so nobody spends an evening on it.

In BLE a peripheral cannot initiate a connection. The mouse advertises; **we**
are the central and we issue `LE_Create_Connection`. `bpc_Trusted` is read in
exactly one place that decides anything:

```c
/* hwconn.c:2262, inside case HC_EVT_CONN_REQUEST: */
if((bd->bd_Flags & BDFF_REGISTERED) || bd->bd_PoPoCfg.bpc_Trusted || (bth->bth_Flags & BTHF_DISCOVERABLE))
    accept = TRUE;
```

`HC_EVT_CONN_REQUEST` is the **BR/EDR** Connection Request event — the lines
above it reject anything that is not `LINKTYPE_ACL`, and an LE link never
raises it. So it is inert here twice over: this device never takes that path,
and if it did, the condition is an `||` whose first term is already true.
Turning it on would only widen what we accept over BR/EDR.

# Why it fails on this board

Two things, and only the second is fixable cheaply.

**There is one opportunity and no second.** The background scan sees an advert,
`bConnAdvertising()` (`hwconn.c:854`) acts on it. No retry, no backoff, no
recovery. Three of that function's four exits are silent, which is why the log
is empty rather than informative. See ISSUE-0061 §1 and §4.

**The scan listens 2.3% of the time.** `bBgScanUpdate()` (`hwtask.c:755`):

```c
bt_buf_writer_write_u8(&w, 0x00);     /* passive */
bt_buf_writer_write_le16(&w, 0x0800); /* interval 1.28 s */
bt_buf_writer_write_le16(&w, 0x0030); /* window 30 ms */
```

30 ms in every 1280. For comparison, Linux's defaults
(`include/net/bluetooth/hci_core.h`): background on an awake machine is
`0x0060 / 0x0030` — **50%**; while establishing a connection, `0x0060 / 0x0060`
— **100%**. The 1.28 s interval appears in exactly one place there: the
parameters used while the machine is **suspended**.

We are running a sleeping host's duty cycle on an awake, mains-powered desktop.
A device advertising every ~1 s has roughly a 2.3% chance per advert of being
heard, and one that only advertises in a short burst on returning to a channel
can be missed entirely — silently, which matches exactly what was measured.

# Why the controller cannot take over here

The mechanism other stacks use — filter accept list plus
`LE_Create_Connection` with filter policy 0x01, so the controller watches
continuously — needs the controller to resolve the peer's private address,
which needs the **Resolving List**, which is LE Privacy 1.2, i.e. Bluetooth
4.2.

This radio reports `HCI 7.0` (Core 4.1) and `HCI 4.1, LMP 4.1 (Broadcom)`. A
BCM43438 is a 4.1 part. **Host-side scanning is the only mechanism this board
has**, and the duty cycle is therefore the only lever. Our software resolution
(`bResolvePrivateAddr()`, `hwtask.c:176` — read line by line and correct) is
not a shortcut; it is what makes reconnection possible at all on a 4.1 radio.

Worth confirming by `LE_Read_Local_Supported_Features` rather than by version
number, but the part number already says it.

# What to try before writing code

Both are free and they separate the remaining hypotheses:

1. **Return to the channel, then click or move the mouse.** Many peripherals
   only advertise on user activity. If it connects then, the advertising window
   is the problem and not our handling of it.
2. **Return to the channel and wait two to five minutes, untouched.** If it
   eventually connects, the duty cycle is proven and step 1 of the plan below
   is the whole fix. If it never connects, the advert is being discarded by one
   of the silent returns, or the scan is not running.

Also confirm, in the same session, that the **Options** page of the Bluetooth
prefs has the global `bgc_AutoConnect` on. It is a second switch, separate from
the per-device one already known set, and `bBgScanNeeded()` returns `FALSE`
without it.

# The plan for this board

In order, cheapest first:

1. **Raise the background scan duty cycle** to the awake-machine values
   (`0x0060 / 0x0030`). One edit. It serves this board and is correct
   generally — nothing here should be scanning at suspend rates.
2. **Add the retry that does not exist**: a periodic re-attempt with a backoff,
   and an active-discovery recovery, so a missed advert is not permanent. This
   is the legacy tree's shape, generalised — ISSUE-0061 §1.
3. **Make the silent paths speak**, throttled to one line per state change:
   whether the background scan is on, and why an advert was discarded, with
   `cn_State` and `cn_WaitAdv`. `hwconn.c` is upstream's, so this is a numbered
   patch, kept until it has answered.
4. Fix `BTIOERR_DISCONNECTED` in bthid's link-down branch (below), because the
   misleading line it produces will confuse every future capture.

Steps 1 and 2 belong upstream once they work here.

# Two smaller defects found along the way

**bthid treats a clean disconnect as an unexpected error** (`bthid.class.c:882`):

```c
else if((ioerr == IOERR_ABORTED) || (ioerr == BTIOERR_NOTCONNECTED))
{
    /* link down: the next read waits for the device to come back
       (BCHA_AutoConnect), do not spin meanwhile */
    btDelayMS(1000);
} else {
    ... "Reading input reports failed: %s (%ld)." ...
    btDelayMS(250);
}
```

`BTIOERR_DISCONNECTED` (28) — "link dropped while the request was pending" — is
missing from that condition, so a remote disconnect takes the else branch: a
warning that should not exist, and 250 ms where the comment intends 1000 ms.
Probably not the cause, since the read is re-issued either way and the *second*
read waits instead of failing, which is what creates the connection in
`HCNS_CONNECTING` with `cn_WaitAdv`.

**`AddBTHardware` now reports failure.** With `BTStackLoader` running first it
restores the radio from the saved config and brings it up, so the
`AddBTHardware` that follows finds it present:

    Adding hardware DEVS:Bluetooth/bthciuart.device, unit 0...failed!
    5-bluetooth.library: Hardware DEVS:Bluetooth/bthciuart.device/0 is already in use.

Harmless — the radio reaches `ready` either way — but the message is a lie, and
on a card with no saved config the two swap roles.

# What voids earlier observations

**No card before 2026-08-26 could run the background scan at all.** The
Broadcom firmware loader never registered, so once it started registering the
bring-up ended in `hc_BringupFailed` — and `bBgScanUpdate()` vetoes the scan on
exactly that flag. Anything recorded before that boot says nothing about this
issue.

**The controller's address changed.** Without the patchram a BCM43430A1 reports
the ROM placeholder `AA:AA:AA:AA:AA:AA`; with it, the real `B8:27:EB:34:97:7D`.
Every pairing made before the fix was against a host identity that no longer
exists, on both sides. The device under test was re-paired afterwards, so the
measurements above are sound; older ones are not.

# Correction to an earlier note

A previous revision claimed that unpersisted services would leave an inbound
reconnect with its HID channels refused at `channel_manager.c:252`.

The persistence part is true — `config.c` stores registrations and bond keys,
not services. The consequence is not, for this bearer: `bConnUp()` re-enumerates
on every fresh connection (`ENUM_GATT_CONNECT` for LE, `ENUM_SDP_CONNECT`
otherwise), and an LE HID device carries its reports over GATT notifications
rather than L2CAP channels opened *to* us. That window only matters for a
BR/EDR peer that pages us and opens PSM 0x11/0x13 before anything has
enumerated it. Worth remembering for a classic keyboard; not what happens here.

# Related

- [ISSUE-0061](ISSUE-0061.md) — the stack-level design gaps this exposed.
- [ISSUE-0060](ISSUE-0060.md) — Bluetooth audio, raised in the same session,
  backlog.
- [ISSUE-0046](ISSUE-0046.md) — the Pi 3 UART controller against the native
  stack; this is downstream of it.
