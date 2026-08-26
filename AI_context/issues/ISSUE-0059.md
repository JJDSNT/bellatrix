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
  - low-energy
  - raspberry-pi-3
blockers:
  - "a BTErrorLog capture taken across a channel switch — see 'The probe'"
related_files:
  - external/aros/rom/bluetooth/bluetooth/hwconn.c
  - external/aros/rom/bluetooth/bluetooth/hwtask.c
  - external/aros/rom/bluetooth/classes/bthid/bthid.class.c
  - AI_context/issues/ISSUE-0046.md
  - AI_context/issues/ISSUE-0058.md
---

# Summary

A Bluetooth mouse with several host channels — the kind that remembers three
hosts and switches with a button — does not come back when its channel is
selected again.

**Reconnection itself works.** Measured on a Pi 3 on 2026-08-26: after a reboot
the device reconnects on its own. Persistence, private-address resolution and
the background scan are therefore all functioning. What fails is specifically
the case where a live connection dropped and the device later returns.

That difference is the whole issue, and it points somewhere narrow.

# The device

From the Bluetooth prefs device window:

- **Low Energy only.** Stored keys are `LE key (legacy)` and `IRK`; there is no
  `BR/EDR link key`. The string comes from `prefs/DevWinClass.c:89-91`.
- state: `registered bonded`
- per-device **auto-reconnect: on** (`bpc_AutoConnect`)
- per-device **trusted: off** (`bpc_Trusted`)

# Who initiates, and why Trusted is a red herring

An earlier revision of this issue listed `Trusted` as the first thing to check.
That was wrong, and it is worth writing down why so nobody spends an evening on
it.

In BLE a peripheral cannot initiate a connection. The mouse advertises; **we**
are the central and we issue `LE_Create_Connection`. So "the device connects
back to us" is never literally true on this bearer — the reconnection is always
our scan seeing an advert and acting on it.

`bpc_Trusted` is read in exactly one place in the whole stack that decides
anything:

```c
/* hwconn.c:2262, inside case HC_EVT_CONN_REQUEST: */
if((bd->bd_Flags & BDFF_REGISTERED) || bd->bd_PoPoCfg.bpc_Trusted || (bth->bth_Flags & BTHF_DISCOVERABLE))
    accept = TRUE;
```

Every other hit (`bluetooth.library.c:1312`, `objects.c:250`, `DevWinClass.c`)
is storage, packing or the checkbox. And `HC_EVT_CONN_REQUEST` is the BR/EDR
Connection Request event — the lines above it reject anything that is not
`LINKTYPE_ACL`, and an LE link never raises it at all.

So Trusted is inert here twice over: this device never takes that path, and if
it did, the condition is an `||` whose first term (`BDFF_REGISTERED`) is
already true. Leaving it off is correct; turning it on would only widen what we
accept over BR/EDR.

# What has to happen for the mouse to come back

1. **The background scan must be running.** `bBgScanNeeded()` (`hwtask.c:722`)
   requires *all* of:
   - `bth_State == BHS_READY` and `BTHF_LE`;
   - **`bgc_AutoConnect`** — the global "reconnect registered devices" on the
     prefs Options page. This is a *second* switch, separate from the
     per-device one, and either alone is not enough;
   - a device with `BDFF_LE | BDFF_REGISTERED | BDFF_BONDED` all set, not
     currently connected;
   - `bpc_AutoConnect` on that device, or a connection already waiting on an
     advert (`cn_WaitAdv`).

   `bBgScanUpdate()` (`hwtask.c:755`) then vetoes it while discovering,
   while another LE scan is active, while `hc_Connecting` is set, or when
   `!hc_BringupDone || hc_BringupFailed`.

2. **The advert must resolve to the device.** A bonded LE peer that gave us its
   IRK advertises from a resolvable private address that changes every few
   minutes — which is exactly what a multi-channel mouse does on returning.
   `bResolvePrivateAddr()` (`hwtask.c:176`) checks `ah(IRK, prand)` against
   every stored IRK and records the current address in `bd_CurAddr`. Read it
   line by line on 2026-08-26: the padding, the prand byte order, the IRK
   LSB→MSB reversal and the three hash bytes compared are all correct. This is
   not where it fails.

3. **`bConnAdvertising()` must decide to connect** (`hwconn.c:854`), reached
   from `hwtask.c:383` for any registered LE device seen advertising.

# Leading hypothesis: the silent return in bConnAdvertising()

```c
if(hc->hc_Connecting || (bth->bth_Flags & BTHF_DISCOVERING)) {
    return;                       /* one connect at a time */
}
if(cn) {
    if((cn->cn_State == HCNS_CONNECTING) && cn->cn_WaitAdv) {
        cn->cn_WaitAdv = FALSE;
        "... is awake - connecting."
        bStartNextConnect(hc);
    }
    return;                       /* any other state: advert dropped, no log */
}
if(!(bd->bd_Flags & BDFF_BONDED) || !bd->bd_PoPoCfg.bpc_AutoConnect ||
   !BluetoothBase->bt_GlobalCfg->bgc_AutoConnect) {
    return;
}
"... is awake - reconnecting."
bEnsureConnection(hc, bd, BDLT_LE, CONN_NOW, &pending, &err);
```

If a connection object exists in **any** state other than *CONNECTING with
`cn_WaitAdv` set*, every future advert is discarded without a single line of
log, indefinitely.

That fits the observed split precisely:

- **after a reboot** there is no `cn` at all, so the code falls through to the
  auto-reconnect branch — and reconnection works, as measured;
- **after a channel switch** there is one, because bthid keeps its binding
  alive on purpose (`bthid.class.c:496`: *"the binding stays; its read channels
  fail and are re-issued when the device reconnects (BCHA_AutoConnect)"*) and
  re-issues its reads, which creates a connection object that can end up in a
  state this branch does not handle.

`hc_Connecting` is a second candidate for the same shape, though `bConnDown()`
(`hwconn.c:605`) does clear both it and `BD_CONN(bd, ...)` before freeing the
connection, so nothing dangles there.

# The probe

Switch the mouse to another channel and back, then read `BTErrorLog`. The
stack emits these, in this order, when it works:

| # | line | proves |
|---|---|---|
| 1 | `Disconnected from <name> (<reason>).` | our side saw the link drop |
| 2 | *(silence — the background scan logs nothing)* | — |
| 3 | `<name> is awake - connecting.` or `- reconnecting.` | advert arrived, IRK resolved, decided to connect |
| 4 | `Connection to <name> failed (<reason>).` | it tried and failed |

Reading:

- **neither 1 nor 3** — we never saw the disconnect; a different problem.
- **1 but not 3** — the silent `return` above, or the scan never re-armed.
  This is the expected outcome if the hypothesis is right.
- **3 then 4** — the advert arrives and the connect itself fails; look at
  `bEnsureConnection()`.
- **3 and then nothing** — the connect is stuck at the controller.

If it is the second case, the next step is one `bug()` line printing
`cn->cn_State` and `cn_WaitAdv` at that `return`, which names the stuck state
outright.

Check the prefs **Options** page in the same session: `bgc_AutoConnect` must be
on, and it is not the per-device switch already confirmed set.

# Two things that invalidate earlier observations

**Every card before 2026-08-26 could not do LE reconnection at all.** The
Broadcom firmware loader never registered (see the `brcmfw.fwl` naming defect
fixed that day), so the controller's bring-up ended in `hc_BringupFailed` once
the loader *did* start registering — and `bBgScanUpdate()` vetoes the
background scan on exactly that flag. Any behaviour recorded before that boot
says nothing about this issue.

**The controller's address changed.** Without the patchram a BCM43430A1 reports
the ROM placeholder `AA:AA:AA:AA:AA:AA`; with it, the real `B8:27:EB:...`. Every
pairing made before the fix was against a host identity that no longer exists,
on both sides. The device under test was re-paired after the fix, so the
measurements above are sound; older ones are not.

# Correction to an earlier note

A previous revision claimed that services and endpoints are never persisted, so
after a reboot an inbound reconnect would get the ACL accepted and then the HID
channels refused at `channel_manager.c:252`.

The persistence part is true — `config.c` stores registrations and bond keys,
not services. The consequence is not, for this bearer: `bConnUp()` re-enumerates
on every fresh connection (`cn_EnumState = ENUM_GATT_CONNECT` for LE,
`ENUM_SDP_CONNECT` otherwise), and an LE HID device carries its reports over
GATT notifications rather than L2CAP channels opened *to* us. The empty-service
window only matters for a BR/EDR peer that pages us and opens PSM 0x11/0x13
before anything has enumerated it. Worth keeping in mind for a classic
keyboard; not what is happening here.

# Related

- [ISSUE-0046](ISSUE-0046.md) — the Pi 3 UART controller against the native
  stack; this is downstream of it.
- [ISSUE-0058](ISSUE-0058.md) — the pin bumps and what they landed on.
