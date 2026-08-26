---
id: ISSUE-0062
title: "The machine hangs within five seconds of a bonded LE device leaving and returning"
status: fix-pending-verification
priority: critical
type: defect
owner: unassigned
created_at: 2026-08-26
updated_at: 2026-08-26
tags:
  - bluetooth
  - bthid
  - low-energy
  - reconnection
  - hang
  - raspberry-pi-3
blockers: []
related_files:
  - external/aros/rom/bluetooth/bluetooth/hwconn.c
  - external/aros/rom/bluetooth/classes/bthid/bthid.class.c
  - patches/aros/0081-bluetooth-do-not-drain-cn-waitreqs-in-place.patch
  - tests/gl/btwatch
  - AI_context/issues/ISSUE-0059.md
---

# Summary

A bonded LE mouse leaves (its channel is switched away) and comes back. Within
five seconds of the link dropping, **the machine stops**. Not the mouse, not
the input system — the machine.

This supersedes the premise of [ISSUE-0059](ISSUE-0059.md), which was written
as "the mouse does not come back, silently". It comes back perfectly well the
first time. What produced the silence in every earlier report was a dead
machine, and every explanation built on that silence — scan duty cycle,
unlogged early returns in `bConnAdvertising()`, the 4.0/4.2 accept-list ladder
— was explaining a symptom that does not exist.

# The measurement

`tests/gl/btwatch` runs detached from the Startup-Sequence just before
Wanderer, and every five seconds writes a heartbeat plus a drain of the
Bluetooth error log. The heartbeat goes to **both** `SYS:btwatch.log` and
`DEBUG:`; the error-log dump only to the card.

The heartbeat is the instrument. A task that only sleeps and prints cannot be
starved into silence by a busy driver — Exec would still schedule it. So
heartbeats that continue mean the machine lives; heartbeats that stop mean it
does not. That distinction was unavailable before and is the whole point of the
probe.

## What came back, 2026-08-26

Card (`SYS:btwatch.log`), heartbeats every five seconds:

```
==== btwatch start ====
00:00:25   first LE Meta subevent 0x02 (legacy advertising report)
00:00:30
00:00:35
00:00:40   BT 5.0 Mouse is awake - reconnecting.
           Connected to BT 5.0 Mouse (LE link, handle 0040, central).
           BT 5.0 Mouse: encrypting the link with the stored LE key.
           BT 5.0 Mouse: link encrypted with the stored key.
           BT 5.0 Mouse: 4 service(s) found.
           bthid.class: HID input from 'BT 5.0 Mouse' connected to input.device.
           notifications enabled on Report (handle 0015, HID report)
           notifications enabled on Report (handle 0019, HID report)
00:00:45
00:00:50
00:00:55
00:01:01
00:01:06   Disconnected from BT 5.0 Mouse (remote user terminated connection).
           bthid.class: Reading input reports failed: link dropped (28).
00:01:11
           <nothing>
```

Serial, the same run: `[btwatch] tick` at 00:00:25, 30, 35, 40, 45, 50, 55,
01:00, 01:06, **01:11**, then nothing.

# What this establishes

**1. It is a hang, not starvation.** Both channels stop at the same heartbeat.
An interrupt storm or a busy driver would slow the ticks, not silence them.
Total silence on an independent sleeping task means interrupts off, a
`Disable()`/`Forbid()` that never returns, or a closed loop with IRQs masked.

This retires the hypothesis that the PL011 receive handler storms when its ring
fills (`aros/arch/m68k-emu68/soc/bluetooth/btuart_init.c:289-296`, the `break`
that leaves the FIFO non-empty before acknowledging). That path is still wrong
and still worth fixing, but it is not this.

**2. Serial output survives the Wanderer takeover.** Every heartbeat appears on
the serial line *after* `[BootUI] display takeover`. The long-standing reading
that "the log ends at the takeover" was wrong: it ended because nothing was
printing. The serial line is a working diagnostic channel and should be used as
one.

**3. The reconnection path is complete and correct.** At 00:00:40 the whole
chain runs: advert heard, matched to the bonded device, connected, link
encrypted with the stored LE key, four services enumerated, bthid attached to
`input.device`, HID report notifications enabled on two handles. Nothing in
that sequence needs designing.

**4. The window is under five seconds**, between the last surviving heartbeat
and the next that never came, immediately after `link dropped (28)`.

# Root cause, found 2026-08-26

`bConnUp()` drains the queue of requests a class parked while the link was
down, in place (`hwconn.c:571-580` before the fix):

```c
while((mn = (struct MinNode *) RemHead((struct List *) &cn->cn_WaitReqs))) {
    struct BtChannel *bch = BCH_FROM_QNODE(mn);
    bch->bch_Flags &= ~BCHF_QUEUED;
    if(!bConnHandleRequest(hc, bch)) { ... }
}
```

`bConnHandleRequest()` puts a request straight back on the same list whenever
the GATT client is busy (`hwconn.c:3078-3081`, the `BEPT_GATT_CHAR` case):

```c
if(cn->cn_CtrlReq || cn->cn_CCCDBusy || (cn->cn_EnumState != ENUM_IDLE)) {
    bReqQueueAdd(&cn->cn_WaitReqs, bch);
    return(TRUE);
}
```

`RemHead()` takes it off the head, the body puts it back on the tail,
`RemHead()` takes it again. The loop cannot terminate, and nothing inside it
can terminate it: clearing `cn_CtrlReq` needs the GATT connect to complete,
which needs the task that is spinning.

## Why it looked intermittent

It only bites on a **reused** connection object.

| path | connection object | `cn_WaitReqs` on connect |
|---|---|---|
| `cn == NULL` → `CONN_NOW`, logs *"is awake - reconnecting"* | freshly allocated | empty — loop never runs |
| `cn_WaitAdv` → `bStartNextConnect`, logs *"is awake - connecting"* | reused, alive since the disconnect | every read the class re-issued meanwhile |

`bthid` re-issues its read every 250 ms while the link is down
(`bthid.class.c:886-899`), and each one is parked with `*pending = TRUE`. The
seven seconds between the disconnect at 00:00:51 and the reconnect at 00:00:58
are roughly twenty-eight of them, times the number of read buffers. The first
one drained starts the GATT connect and sets `cn_CtrlReq`; the second finds it
busy and closes the loop.

## How the evidence matches

- The hang is always between `Connected to ...` (`hwconn.c:566`) and
  `... encrypting the link with the stored LE key` (`hwconn.c:2085`, reached
  via `bLEReencrypt()`) — the two lines that bracket the loop.
- The last heartbeat printed its `Echo` but never its `Date`: the machine died
  between two commands, not inside a wait.
- The connection at 00:00:35 of the same run, through the *"reconnecting"*
  branch, completed in full — encrypt, four services, notifications on two
  handles.
- The run of 2026-08-26 that was first read as a successful reconnection ended
  on exactly the same line. It had hung too; the reading was wrong.

# The fix

`patches/aros/0081-bluetooth-do-not-drain-cn-waitreqs-in-place.patch`.

The correct pattern was already in the file twenty lines above:
`bDispatchWaiting()` (`hwconn.c:446-461`) detaches the whole queue onto a local
list before walking it, so anything re-queued lands on a now-empty
`cn_WaitReqs` and is handled on the next event instead of re-entering the loop.
`bConnUp()` now calls it instead of open-coding the drain.

Builds clean (`make kernel-bluetooth-library`). **Not yet verified on
hardware** — that is what this issue is waiting on. The test is the one that
produced it: reboot so the bond is restored, connect, switch the channel away,
switch back, and repeat several times in one session.

# What this does to the other issues

- [ISSUE-0059](ISSUE-0059.md) — premise void; blocked on this.
- [ISSUE-0061](ISSUE-0061.md) — the four design gaps stand on their own merits,
  but gap 4's justification ("an empty log cannot distinguish ...") was written
  about this hang and has been corrected there.
