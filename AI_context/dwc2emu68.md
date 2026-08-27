---
title: "What the Bellatrix DWC2 driver is for, and how to tell it is better"
status: active
updated_at: 2026-08-26
components: [usb, dwc2, m68k-emu68]
---

`usb2otg` works. That is the awkward part of replacing it: a rewrite that ends
up the same shape has cost effort and bought nothing. This file says what
"better" means for `dwc2emu68` in terms that can be checked, so the work can
be steered by a target instead of by whatever the last serial log complained
about.

It is not a status report. `AI_context/issues/ISSUE-0047.md` carries the
chronology; this carries the destination.

## What is actually wrong with the driver being replaced

Not its behaviour — it enumerates devices. Its **state model**.

A transfer's state in `usb2otg` lives in several places at once: a channel
array entry, one of several queues, a per-device NAK gate, a delayed-channel
tick, a split-state field, a pending flag. The invariants between them are
maintained by hand at dozens of call sites, and nothing can check them. The
machinery that follows is the consequence, not the cause:

| | sites |
|---|---|
| fixed channel roles (`CHAN_CTRL_SPLIT`, `CHAN_INT1..CHAN_INT_LAST`) | 19 |
| quarantine and recovery (`hu_DeadChannels`, `exorcise`, blackout, core-reset recovery) | 46 |

A driver needs to quarantine channels because it cannot tell whether a channel
is sound. It needs a core-reset recovery path because it cannot tell whether
its own bookkeeping is sound. The `IIntXFerQueue walk runaway — list cycle`
that this port has been seeing for months is the same thing surfacing: a node
reachable from two lists, because two places both believed they owned it.

Totals: 9280 lines against our 3110. The size follows the state model too.

## The five invariants

These are the deliverable. Each is checkable, and each rules out a class of
defect rather than fixing an instance of it.

**1. A transfer has exactly one owner.** It is on a queue, or it owns a
channel. Never both, never neither, and every transition between the two goes
through one function. *Consequence:* a half-owned channel cannot exist, so
there is nothing to quarantine. *Acceptance:* the driver never resets the core
to recover. If it ever needs to, this invariant has failed and the answer is
to find out where, not to add a recovery path.

**2. A channel is a resource, not a role.** Any of the eight serves any
transfer type, allocated on demand. *Consequence:* concurrency is the default
rather than something to be arranged; a control transfer does not wait for a
channel that interrupt traffic has reserved.

**3. The split sequencer owns every ending of a split.** A split transaction
can end ten ways — `XFERCOMP`, `NYET`, `NAK`, `STALL`, `XACTERR`, `BBLERR`,
`DATATGLERR`, `FRMOVRUN`, a bare `CHHLTD`, `AHBERR` — and all of them leave
the channel's split engine mid-sequence. They must leave through one exit that
scrubs it. *Consequence:* "the one unclean ending that skipped the reset",
which is upstream's own description of a bug it shipped, stops being
expressible.

**4. Deadlines belong to the interrupt; policy belongs to the task.** A
translator's result window is measured in microframes and a task round trip is
not, so accepting a start-split and going back for the result run in the top
half. Everything that can wait — queueing, completion, error reporting — runs
in the task. *Acceptance:* nothing on the interrupt path allocates, takes a
list, or prints.

**5. A transfer ends when the device says so, not when a transaction does.**
A split moves one packet per transaction, so behind a translator every
multi-packet transfer comes back in pieces, each reported as `XFERCOMP` with
packets still outstanding. The only thing that ends a transfer early is a
short packet, and short is measured against MaxPktSize -- never against
whatever this arming happened to ask for. Every stage that can be cut this way
advances `iouh_Actual` and re-arms.

*Consequence:* what this rules out is silent and speed-dependent. A full-speed
device with a 64-byte endpoint reads an 18-byte descriptor in one packet and
works; a low-speed one with an 8-byte endpoint gets 8 bytes and a truncated
descriptor, and can never read anything longer. From the desktop that looks
like a device named by vendor and product id that does not respond -- four
layers away from the test that conflated the two questions.

**6. Diagnostics must not perturb what they measure.** One line at 115200 baud
is about nine milliseconds. A trace inside a split's timing window guarantees
the split fails, which is not a hypothetical: it happened here, and it cost
several rounds of hardware testing. *Consequence:* counters on hot paths,
prints only where the outcome is already decided.

## Where the code stands

| Invariant | State |
|---|---|
| 1. One owner | Holds. `channel_release()` is the single transition and no second owner exists. Untested under concurrency. |
| 2. Channel as resource | Holds. `channel_alloc()`; no role is pinned anywhere. |
| 3. Split owns its endings | **Broken by construction** — see below. |
| 4. Deadlines in the interrupt | Holds for splits since the sequencer moved there. |
| 5. Clamped stages continue | Holds for bulk, isochronous and, since 23:10, the control data stage. |
| 6. Non-perturbing diagnostics | Partly. The split path is silent; the arm/completion trace is still on the transfer path and capped by a count. |

### Invariant 3 is the open structural defect

`channel_irq()` dispatches in this order:

```
1  isochronous errors            -> finish
2  XACTERR, three attempts       -> re-arm
3  STALL|AHBERR|XACTERR|BBLERR|DATATGLERR -> finish
4  split_irq()                   <-- the split sequencer, fourth
5  NAK
6  bare CHHLTD                   -> scrub, retry
7  XFERCOMP
```

A split's transaction error and a split's STALL are handled at steps 2 and 3
by code that does not know a split exists. The scrub added to
`channel_release()` covers it, but by remembering to — which is the same
guarantee `usb2otg` had, and the same one that failed there.

The fix is dispatch order, not another special case: when `chan->split` is not
`NONE` the interrupt belongs to the split state machine, and that machine
enumerates all ten endings and has one exit.

## What is left, in order

1. **Invariant 3.** Dispatch splits first; enumerate the endings; one exit.
   Removes the `channel_release()` scrub as a special case.
2. **Periodic completions arrive by watchdog, not by interrupt.** A low-speed
   mouse works, but 512 watchdog recoveries were counted on its interrupt
   endpoint in one boot -- each one a channel that reached a terminal state
   whose interrupt never reached the unit task. The periodic path is running
   at the 10 ms watchdog's cadence instead of the endpoint's 2 ms interval.
   Control transfers on the same channel do reach the task, and `HAINTMSK`
   and `GINTMSK.HCHINT` are both re-armed by `arm()`, so the cause is not
   obvious and must not be guessed at.

3. **Periodic interval.** `park_periodic()` clamps the interval to
   `DWC2_FRAME_MASK`, which is the frame counter's modulus, not a period
   limit. An endpoint asking for 2048 frames gets `due = frame - 1` and is
   polled every frame — observed as `interval=2047 due=1059 frame=1060`.
   A period longer than the counter needs a repeat count, not a clamp.
4. **Concurrency.** Every hardware trace so far shows `chan=0`, because
   Poseidon enumerates serially. The two in-flight guards (same endpoint, same
   translator) have never been evaluated. Needs two devices generating traffic
   at once.
5. **PING for high-speed bulk OUT.** Not implemented. Bulk OUT to a
   high-speed device will NAK repeatedly without it.
6. **Isochronous.** Implemented, never exercised on hardware.

Deliberately not on this list: channel quarantine, dead-channel tracking, and
a core-reset recovery path. If any of them starts to look necessary, that is
invariant 1 failing and the finding is where, not what to add.
