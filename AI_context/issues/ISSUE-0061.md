---
id: ISSUE-0061
title: "The Bluetooth stack reconnects by luck: no scan policy, no retry, no controller-capability layer"
status: backlog
priority: medium
type: refactor
owner: unassigned
created_at: 2026-08-26
updated_at: 2026-08-26
tags:
  - bluetooth
  - low-energy
  - reconnection
  - design
  - upstream
blockers: []
related_files:
  - external/aros/rom/bluetooth/bluetooth/hwtask.c
  - external/aros/rom/bluetooth/bluetooth/hwconn.c
  - AI_context/issues/ISSUE-0059.md
---

# Summary

Four gaps in how the stack reconnects a bonded LE device. None of them is
specific to the Raspberry Pi 3; they would bite any host with any radio. They
were found while chasing a concrete failure on a Pi 3 — a multi-channel mouse
that does not come back — which is [ISSUE-0059](ISSUE-0059.md). That issue
carries the fix for *this* board; this one carries what the stack should
eventually look like.

Worth taking upstream: the stack lives in `rom/bluetooth` in the AROS tree, and
gap 3 in particular is about hardware this port will never see.

# 1. One shot, and no second

A returning device gets exactly one opportunity: the background scan sees an
advert, `bConnAdvertising()` (`hwconn.c:854`) acts on it. If that is missed —
because the scan was between windows, or because the connection object was in a
state the function does not handle — nothing else ever happens.

There is no retry, no backoff, and no recovery path.

The shape that is missing is not hypothetical; the pre-2026-08-03 tree had it
(`~/bellatrix-legacy/src/io/bluetooth/bt_host.c:1339`), over BTstack and
BR/EDR:

```
PASSIVE      -> once a deadline passes, try connecting to the known pairs
CONNECTING   -> with a timeout; on expiry fall through to DISCOVERING
DISCOVERING  -> start an *active* discovery for the known device, 30 s
                found   -> back to PASSIVE, which connects
                expired -> BACKOFF 60 s, then round again
```

Periodic retry, an active-discovery recovery, and a backoff. The current stack
has none of the three. This is knowledge the project already had and lost in
the crossing to the new stack.

# 2. There is no notion of a scan *scenario*

`bBgScanUpdate()` (`hwtask.c:755`) writes two constants by hand:

```c
bt_buf_writer_write_le16(&w, 0x0800); /* interval 1.28 s */
bt_buf_writer_write_le16(&w, 0x0030); /* window 30 ms */
```

Linux keeps five sets and picks between them
(`include/net/bluetooth/hci_core.h`, assigned in `hci_alloc_dev_priv()`):

| scenario | interval | window | duty |
|---|---|---|---|
| discovery | 0x0012 (11.25 ms) | 0x0012 (11.25 ms) | 100% |
| background, awake | 0x0060 (60 ms) | 0x0030 (30 ms) | 50% |
| establishing a connection | 0x0060 (60 ms) | 0x0060 (60 ms) | 100% |
| suspended | 0x0800 (1.28 s) | 0x0012 (11.25 ms) | 0.9% |
| advertisement monitor | 0x0060 (60 ms) | 0x0030 (30 ms) | 50% |

BlueZ exposes them as `ScanInterval*` / `ScanWindow*` pairs in `main.conf` for
the same reason.

What is missing here is not the numbers — those are one edit — but the layer
that knows there is a choice to make and what it depends on. As written, an
awake mains-powered desktop scans at the duty cycle Linux reserves for a
suspended laptop, and nothing in the code can express that this is wrong.

# 3. No controller-capability layer

The stack never asks the radio what it can do, so it always does the least
capable thing.

The mechanism every other stack converges on is to stop sampling in the host
entirely: put the peer in the controller's **filter accept list** and issue
`LE_Create_Connection` with filter policy 0x01 — the Link Layer's Auto
Connection Establishment procedure — so the controller watches continuously and
reports a connection when it happens. That is Zephyr's `bt_conn_le_create_auto()`
and BlueZ's kernel auto-connect.

For a peer using a resolvable private address it also needs the controller's
**Resolving List** (`LE_Add_Device_To_Resolving_List`,
`LE_Set_Address_Resolution_Enable`), because the accept list matches on the
advertised address. That is LE Privacy 1.2, i.e. **Bluetooth 4.2**.

So there are two correct implementations, chosen by what the radio supports:

- **4.2 and later** — hand the IRKs to the controller, put the peer in the
  accept list, let the Link Layer do it. No host window to miss.
- **4.1 and earlier** — resolve private addresses in software on the host
  (`bResolvePrivateAddr()`, `hwtask.c:176`, which this stack already does
  correctly) and scan from the host, with a duty cycle chosen for an awake
  machine.

Today only the second exists, and it exists implicitly rather than as a
decision. A Pi 3's BCM43438 is 4.1, so this port would take that branch anyway
— but the stack is in the AROS tree and will meet 4.2, 5.x and every USB dongle
in existence.

The capability check should be a real query — `LE_Read_Local_Supported_Features`
and the supported-commands bitmap — not an inference from the HCI version.

# 4. It cannot say why it did not reconnect

`bConnAdvertising()` has four exits and three of them are silent:

```c
if(hc->hc_Connecting || (bth->bth_Flags & BTHF_DISCOVERING))   return;  /* silent */
if(cn) {
    if((cn->cn_State == HCNS_CONNECTING) && cn->cn_WaitAdv) { ...logs... }
    return;                                                             /* silent */
}
if(!bonded || !bpc_AutoConnect || !bgc_AutoConnect)            return;  /* silent */
```

Whether the background scan is running is `KPRINTF` only, so invisible in any
normal capture.

The practical consequence was measured on 2026-08-26: a device that does not
come back produces an empty log, and an empty log cannot distinguish "the
advert never arrived" from "it arrived and was discarded, for one of three
different reasons". Hours went into that ambiguity.

Any logging added here needs throttling — one line per state change, never one
per advert, or it floods and buries what it was meant to show.

# Order

1 and 2 are cheap and fix a real defect; they are being done for this board in
[ISSUE-0059](ISSUE-0059.md) and should be generalised from there. 4 is small
and pays for itself the first time something goes wrong in the field. 3 is the
real design work and the one worth proposing upstream.
