---
id: ISSUE-0063
title: "USB2OTG: the interrupt transfer queue develops a cycle, and the fix in the tree is a guard"
status: open
priority: high
type: defect
owner: unassigned
created_at: 2026-08-26
updated_at: 2026-08-26
tags:
  - usb
  - usb2otg
  - dwc2
  - raspberry-pi-3
blockers: []
related_files:
  - aros/arch/m68k-emu68/soc/usb/usb2otg/usb2otg_intr.c
  - aros/arch/m68k-emu68/soc/usb/usb2otg/usb2otg_schedule.c
---

# Summary

`hu_IntXFerQueue` acquires a cross-linked node and the walk over it becomes a
cycle. Seen on hardware 2026-08-26 as a repeating serial line while a USB mouse
was in use:

```
[USB2OTG] SOF: IntXFerQueue walk runaway — list cycle, aborting walk
```

The USB mouse stops working when this happens, because that queue is where its
interrupt-endpoint reports live.

# The corruption is known and unfixed; what is in the tree is a guard

`usb2otg_intr.c:931-941`:

```c
int walk_guard = 0;

ForeachNodeSafe(&USBUnit->hu_IntXFerQueue, req, next)
{
    /* A cross-linked node turns this walk into a cycle (seen
     * hanging the machine); bail loudly instead of spinning. */
    if (++walk_guard > 128)
    {
        bug("[USB2OTG] SOF: IntXFerQueue walk runaway — list cycle, aborting walk\n");
        break;
    }
```

The same guard exists on the watchdog walk (`usb2otg_intr.c:3359-3370`). Both
stop the machine hanging. Neither stops the list being corrupted, and the
comment records that the corruption was already understood when they were
added.

# Where to look first

The wedge-recovery path re-queues each channel's in-flight request without
checking whether the node is still linked somewhere (`usb2otg_intr.c:517-545`):

```c
stuck_req = USBUnit->hu_Channel[i].hc_Request;
if (stuck_req == NULL) continue;
USBUnit->hu_Channel[i].hc_Request = NULL;
...
case UHCMD_INTXFER:
    ADDHEAD(&USBUnit->hu_IntXFerQueue, (struct Node *)stuck_req);
```

`ADDHEAD` of a node that is still in a list is exactly how a cycle is made. The
bulk scheduler is careful about this — `usb2otg_schedule.c:1684-1685` does
`REMOVE(req)` before binding the request to a channel — which is the pattern
this path does not follow. Whether an INT request can reach `hc_Request` while
still linked has not been traced; that is the work.

# Second, smaller

`pl011bt`-style rule broken in the same file: the SOF handler's sibling in
`pl011bt_init.c` documents "no call back into AROS" and then calls `bug()` from
interrupt context. Here the guard does the same. It is bounded, so it is not
the defect, but it is worth removing once the cause is fixed — a `bug()` inside
an interrupt handler that fires on a corrupted list is the worst possible place
for it.

# Relation to Bluetooth

Observed while investigating [ISSUE-0062](../consolidated/history/ISSUE-0062.md), and the user reported
that a USB mouse and a Bluetooth mouse interfere in practice. They do not
interfere in the input path — both classes inject relative mouse events into
`input.device` with `IND_WRITEEVENT` and both chain their `lowlevel.library`
`SetFunction` patches correctly, verified 2026-08-26. Any interaction is below
that layer, and this defect is a candidate for it. The two issues should not be
merged until something connects them.
