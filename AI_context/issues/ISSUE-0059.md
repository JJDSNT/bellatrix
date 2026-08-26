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
  - "does the device reconnect if left for minutes, or after the user clicks it? — see 'What to try before writing code'"
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

**Measured 2026-08-26, second session.** Leaving the channel is clean and
fully observed:

    Disconnected from <name> (remote user terminated connection).
    Reading input reports failed: link dropped (28).

Returning to the channel produces **no log line at all** — neither
`is awake - connecting.` nor `- reconnecting.`. That is the "1 but not 3" case
of the table below, and it is what the rest of this issue is about.

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

# The one shot, and why there is no second

The current stack gives a returning device exactly one opportunity: the passive
background scan sees an advert, `bConnAdvertising()` acts on it. There is no
retry, no backoff, and no recovery path. If that single opportunity is missed
or lands on one of the silent returns, nothing ever happens again and nothing
is logged.

Two things make missing it plausible.

**The scan listens 2.3% of the time.** `bBgScanUpdate()` (`hwtask.c:755`) sets:

```c
bt_buf_writer_write_u8(&w, 0x00);     /* passive */
bt_buf_writer_write_le16(&w, 0x0800); /* interval 1.28 s */
bt_buf_writer_write_le16(&w, 0x0030); /* window 30 ms */
```

30 ms in every 1280 ms. A device advertising fast (30 ms) is caught almost at
once. One that has fallen back to ~1 s advertising has roughly a 2.3% chance
per advert, so tens of seconds on average — and a device that only advertises
for a short burst on returning to a channel can be missed entirely, silently.

**The legacy tree solved the same problem with a state machine.** It ran
BTstack over BR/EDR, so none of the code transfers, but the shape is the point
(`~/bellatrix-legacy/src/io/bluetooth/bt_host.c:1339`):

```
PASSIVE      -> once a deadline passes, try connecting to the known pairs
CONNECTING   -> with a timeout; on expiry fall through to DISCOVERING
DISCOVERING  -> start an *active* 30 s discovery looking for the known device;
                found -> back to PASSIVE, which connects
                expired -> BACKOFF 60 s, then round again
```

Periodic retry, an active-discovery recovery path, and a backoff. The current
stack has none of the three.

The standard answer elsewhere is the controller's own filter accept list:
`LE_Create_Connection` with the whitelist filter policy leaves the controller
watching for the peer continuously, instead of the host sampling 2.3% of the
time and then racing to connect. That is worth researching before choosing a
fix.

# What other stacks do (researched 2026-08-26)

Linux keeps three sets of LE scan parameters, and the numbers are in
`include/net/bluetooth/hci_core.h`:

```c
#define DISCOV_LE_SCAN_INT_FAST     0x0060 /* 60 msec */
#define DISCOV_LE_SCAN_WIN_FAST     0x0030 /* 30 msec */
#define DISCOV_LE_SCAN_INT_CONN     0x0060 /* 60 msec */
#define DISCOV_LE_SCAN_WIN_CONN     0x0060 /* 60 msec */
#define DISCOV_LE_SCAN_INT_SLOW1    0x0800 /* 1.28 sec */
#define DISCOV_LE_SCAN_WIN_SLOW1    0x0012 /* 11.25 msec */
```

`hci_alloc_dev_priv()` assigns them:

```c
hdev->le_scan_interval        = DISCOV_LE_SCAN_INT_FAST;   /* 60 ms  */
hdev->le_scan_window          = DISCOV_LE_SCAN_WIN_FAST;   /* 30 ms  -> 50% duty */
hdev->le_scan_int_connect     = DISCOV_LE_SCAN_INT_CONN;   /* 60 ms  */
hdev->le_scan_window_connect  = DISCOV_LE_SCAN_WIN_CONN;   /* 60 ms  -> 100% duty */
hdev->le_scan_int_suspend     = DISCOV_LE_SCAN_INT_SLOW1;  /* 1.28 s */
hdev->le_scan_window_suspend  = DISCOV_LE_SCAN_WIN_SLOW1;  /* 11.25 ms -> 0.9% duty */
```

So an awake Linux host scans **50%** of the time in the background and **100%**
while establishing a connection. The 1.28 s interval appears in exactly one
place: the parameters used while the machine is *suspended*.

Ours is `0x0800 / 0x0030` — Linux's suspend interval with a wider window, about
**2.3%**. That is the duty cycle of a sleeping host, running on an awake one.
BlueZ exposes these as `ScanIntervalAutoConnect` / `ScanWindowAutoConnect` in
`main.conf` precisely because the auto-connect case wants its own, denser
values.

The other half of what everyone else does is to stop sampling in the host at
all:

- BlueZ auto-reconnects bonded HoG devices with an active scan today, and the
  kernel gained passive scanning plus auto-connect through the controller.
- Zephyr's `bt_conn_le_create_auto()` runs the Link Layer's **Auto Connection
  Establishment** procedure: the peer goes in the filter accept list and
  `LE_Create_Connection` is issued with filter policy 0x01, so the *controller*
  watches continuously and reports a connection when it happens. No host-side
  window to miss.

## Why the controller cannot do it for us on this board

The accept list matches on the advertiser's address. A peer using a resolvable
private address changes that address every few minutes, so the controller can
only filter it if it can resolve it — which needs the **Resolving List**
(`LE_Add_Device_To_Resolving_List`, `LE_Set_Address_Resolution_Enable`), part of
LE Privacy 1.2 and therefore **Bluetooth 4.2**.

This controller reports `HCI 7.0` in the bring-up line, which is Core 4.1, and
the ready line agrees: `HCI 4.1, LMP 4.1 (Broadcom)`. A BCM43438 is a 4.1 part.
So there is no controller-side address resolution here, the accept list cannot
hold an RPA peer, and the Zephyr/BlueZ answer is unavailable to us.

Worth confirming directly rather than by version number — read
`LE_Read_Local_Supported_Features` and the supported-commands bitmap — but if
it holds, then **host-side scanning is the only mechanism this board has**, and
the duty cycle is the only lever. Our own software resolution
(`bResolvePrivateAddr()`) is not a shortcut taken cheaply; it is the thing that
makes reconnection possible at all on a 4.1 radio.

## What that suggests

1. **Raise the background duty cycle** to something like Linux's awake values
   (`0x0060 / 0x0030`, 50%), rather than its suspend values. One line, no new
   mechanism, and it directly addresses the "advert arrives during the 97.7% we
   are not listening" case.
2. **Add the retry that does not exist** — the legacy state machine's shape:
   periodic re-attempt, an active-discovery recovery, and a backoff, so a
   missed advert is not permanent.
3. Consider whether power matters here at all: this is a mains-powered desktop,
   not a phone. The 2.3% figure buys battery life the machine does not need.

# A smaller defect found along the way

bthid's read loop treats a dropped link as an unexpected error:

```c
/* bthid.class.c:882 */
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
missing from that condition, so a clean remote disconnect takes the else
branch: a warning that should not be there, and a 250 ms delay where the
comment intends 1000 ms. It is the exact message seen on the hardware.

Probably not the cause: the read is re-issued either way, and the *second* read
does not fail with 28 (with auto-connect on it waits instead), which is what
creates the connection in `HCNS_CONNECTING` with `cn_WaitAdv` — the state
`bConnAdvertising()` handles. Worth fixing regardless, because the misleading
line will confuse every future capture.

# What to try before writing code

Both are free and they separate the hypotheses:

1. **Return to the channel, then click or move the mouse.** Many peripherals
   only advertise on user activity. If it connects then, the advertising window
   is the problem, not our handling of it.
2. **Return to the channel and wait two to five minutes, untouched.** If it
   eventually connects, the scan duty cycle is the problem, and the fix is a
   wider window or the controller's accept list. If it never connects, it is
   the silent return or the scan is not running at all.

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

The second case is what was measured. The instrumentation that would name it is
three lines, not one, and all three need throttling — one line per state
change, never one per advert, or the log floods and buries what it was meant to
show:

- when the background scan turns on and off (today only `KPRINTF`, invisible in
  a normal capture, so we cannot even tell whether it is running);
- why an advert was discarded, at each of the three silent returns in
  `bConnAdvertising()`, with `cn_State` and `cn_WaitAdv`;
- `bgc_AutoConnect` and `bpc_AutoConnect` as the stack reads them, since the
  global one on the prefs Options page has still not been confirmed set.

`hwconn.c` is upstream's, so this is a numbered diagnostic patch, to be removed
or turned into the real fix once it has answered.

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

# Side effect of the Startup-Sequence reorder

With `BTStackLoader` running first it restores the radio from the saved config
and brings it up, so the `AddBTHardware` that follows finds it already there:

    Adding hardware DEVS:Bluetooth/bthciuart.device, unit 0...failed!
    5-bluetooth.library: Hardware DEVS:Bluetooth/bthciuart.device/0 is already in use.

Harmless — the radio reaches `ready` either way — but the message is a lie, and
on a card with no saved config the two swap roles. Needs cleaning up separately
from this issue.

For the record, the same capture is what confirmed the firmware fixes landed:
the patchram now runs inline at bring-up step 2 (which is why step 2 is absent
from the run of "ok" lines), step 3 continues normally, there is no
re-initialisation, and the controller reports a real address —
`B8:27:EB:34:97:7D` instead of the ROM placeholder.

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
