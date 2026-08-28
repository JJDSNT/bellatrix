---
title: "dwc2emu68 goes deaf while the CPU is busy: the ISR silences the whole controller"
status: in-progress
priority: high
type: defect
updated_at: 2026-08-27
date: 2026-08-27
components:
  - usb
  - dwc2
  - m68k-emu68
blockers: []
related:
  - ISSUE-0047
---

# Symptom

USB input freezes while the machine is drawing: Wanderer redrawing icons, or
the Mesa demo running. The pointer stops, then jumps when the drawing stops.
`usb2otg.device` did not behave this way under the same load.

Reported 2026-08-27: *"o comportamento do novo driver difere do antigo pois ele
fica congelado quando esta havendo algo na tela, como redesenho dos icons ou
rodando o demo mesa"*.

This is the same defect as the one recorded in ISSUE-0047 as **Defect A** --
2048 watchdog recoveries in a boot, every one of them `stage=4` (an interrupt
endpoint), each with a terminal `HCINT` still set (`0x12` NAK|CHHLTD, `0x42`
NYET|CHHLTD). A terminal `HCINT` at watchdog time means the ISR never ran for
that channel. It did not run because it could not.

# Cause

Two decisions compound, and neither is visible on an idle machine.

**1. The interrupt top half clears `GAHBCFG.GLBLINTRMSK`.**
`controller_irq()` (`dwc2emu68_controller.c:115-119`) silences the *entire*
controller before waking the unit task, and only
`dwc2_controller_drain_irq()` (`:376-378`) puts it back -- and that runs in
the unit task. Between those two points the DWC2 raises no interrupt at all.
Not late: deaf.

The gate is not needed for the channel interrupts, which the ISR already
acknowledges per channel (`HCINT(i)` written back, so `HAINT` and therefore
`GINTSTS.HCHINT` drop on their own). It is needed only for the two sources
this driver acknowledges in the task instead: `PRTINT`, which clears when the
port's change bits are written back, and `DISCONNINT`, which clears when
`GINTSTS` is written. Leaving those two asserted would re-enter the handler
forever -- so the fix is to mask those two bits in `GINTMSK`, not to silence
the controller.

**2. The unit task runs at priority -5** (`dwc2emu68_device.c:76`).

Below every ordinary task. A GL demo or an icon redraw at priority 0 preempts
it for as long as it runs, so the window in (1) is not microseconds -- it is
the whole length of the drawing. AROS's own host-controller device,
`rom/usb/vusbhc/vusbhci_device.c:514`, creates its worker tasks at
`TASKTAG_PRI, 5`.

`usb2otg` has neither property: it never touches `GLBLINTRMSK` after init
(`grep GAHBCFG aros/arch/m68k-emu68/soc/usb/usb2otg/*.c` finds nothing), and
its worker task is created with no `TASKTAG_PRI`, i.e. 0. Under load it
therefore degrades to *late*; ours degrades to *stopped*. That is the whole
difference the user is seeing.

## The gate was protecting nothing

Checked while writing the fix: `PRTINT` and `DISCONNINT` are never set in
`GINTMSK`. `dwc2_controller_start()` enables `SOF` only
(`dwc2emu68_controller.c:349`) and `HCHINT` is added and removed around armed
channels (`dwc2emu68_transfer.c`); port changes are picked up by
`dwc2_root_poll()` instead. So the only sources that ever reach
`controller_irq()` today are the two it fully acknowledges itself.

The global gate therefore bought nothing at all and cost the freeze. The
`gintmsk_deferred` bookkeeping is kept because it is the correct contract the
moment `PRTINT` is enabled, not because anything sets it today.

# Fix

> **Hardware regression, 2026-08-28:** removing the global gate made the real
> Pi stop before Wanderer, after enumeration and without a reported transfer
> error. Raising task priority and making SOF event-driven did not restore boot.
> The lock-free gate replacement is therefore withdrawn pending a diagnostic
> that identifies the exact loop or lost transition. The current test build
> restores the proven global gate and changes only the task priority from -5
> to 0. This is a controlled intermediate state, not closure of the defect.
>
> The first attempted control build was not controlled: it also left SOF
> event-driven. That 33,880-byte build booted Wanderer but enumeration stopped
> at address 3 with three control-status `XACTERR|CHHLTD` results, before
> reaching the mouse. While restoring the baseline afterwards, an intermediate
> 33,272-byte build accidentally swapped the two controller-init `GINTMSK`
> writes (SOF during reset, zero after IRQ registration); it was caught before
> hardware testing and must not be used. The replacement restores both writes
> to the known baseline: zero during reset, SOF after IRQ registration.
>
> With the true baseline restored and only priority changed, boot again reached
> Wanderer but enumeration still stopped at address 3, control status, with
> three `XACTERR|CHHLTD` results. This isolates a second effect of removing the
> hot-path trace: the old `arm` line read back several channel registers before
> `CHENA`, draining posted MMIO writes. The next controlled build adds an
> explicit `dsb()` plus `HCTSIZ` readback at that commit point; it does not add
> a guessed time delay or restore serial output.

## Controlled baseline and next test, 2026-08-28

The exact committed driver was rebuilt (`git diff` empty for the driver) and
tested on the Pi. It boots Wanderer, enumerates through address 5, and the mouse
delivers HID reports. Under desktop activity its periodic endpoint falls back
to the watchdog exactly as originally reported: recoveries rise through 1, 2,
4, 8, 16, 32, 64 and 128, all at stage 4, with terminal
`HCINT=0x12` or `0x42`.

The next build changes one line only against that confirmed baseline:
`TASKTAG_PRI` from -5 to 0. The global interrupt gate, SOF policy, channel
release protocol, pending-state handling and all diagnostics remain byte-for-
byte at the committed behavior. This tests the scheduling half of the original
two-factor diagnosis without exposing the unvalidated lock-free IRQ protocol.

The hardware result confirms that one-line change: enumeration and Wanderer
still complete, and opening windows no longer makes the pointer stop. It does
not, however, eliminate the underlying missed channel interrupts: watchdog
recoveries still rose to 1024 at stage 4. Priority 0 makes the gated task run
soon enough that recovery is no longer perceptible; it does not make the ISR
receive those terminal events.

An attempted cleanup of routine submit, completion, SOF, NAK,
watchdog-sampling and HID-data prints was withdrawn before hardware testing.
The successful user-visible result does not close the defect: 1024 real
watchdog recoveries are still evidence that channel interrupts are being lost.
Keep the complete, known trace until that mechanism is fixed and measured;
instrumentation reduction comes afterwards.

The recovery line is extended, without changing control flow, to capture the
whole propagation chain at the instant the watchdog finds terminal HCINT:
`GAHBCFG`, `GINTSTS/GINTMSK`, `HAINT/HAINTMSK` and `HCINT/HCINTMSK`. This
distinguishes a controller still closed by the global gate from a channel
whose terminal state failed to propagate through HAINT or HCHINT.

The Pi result is conclusive: every reported recovery had `GAHBCFG=0xa2`
(`GLBLINTRMSK` clear), while `GINTSTS.HCHINT`, `HAINT`, the channel bit in
`HAINTMSK`, terminal `HCINT` and its `HCINTMSK` bits were all present. Nothing
failed to propagate; delivery was globally gated.

The unit task processed a coalesced timer signal before `irq_pending`. The
watchdog therefore sampled and consumed the terminal channel state while an
earlier IRQ still held the global gate closed, then called that a recovery.
The task now drains/reopens the controller before servicing the watchdog. A
channel that halted during the closed window can then raise its already-pending
hardware IRQ and be acknowledged normally before the watchdog examines it.

Hardware retest showed that ordering change was insufficient. It removed the
case already present in `irq_pending`, but a channel can halt after that first
drain while the task is still running. The watchdog then again observes
`GAHBCFG=0xa2`, `HCHINT`, `HAINT` and terminal `HCINT`. This proves that no
ordering inside the task can make the global-gate protocol sound: the gate
must be removed, because hardware is allowed to complete at every point while
the task owns the CPU.

# Accepted intermediate fix and rejected experiments

The only hardware-confirmed functional correction is to create the unit task
at priority 0 instead of -5. With that one scheduling change, the Pi reaches
Wanderer, enumerates the mouse and keeps pointer input responsive while windows
open and redraw. The original global gate and the full trace remain in place.

Draining an already-published IRQ before servicing a coalesced watchdog signal
is also retained. It avoids misclassifying the snapshot already handed to the
task, but hardware testing proved it cannot close the entire gate window: a
channel may halt after that drain while the task is still running.

The watchdog diagnostic now reports `GAHBCFG`, `GINTSTS/GINTMSK`,
`HAINT/HAINTMSK` and `HCINT/HCINTMSK`. On the working priority-0 build,
periodic recoveries still rise into the hundreds or thousands with
`GAHBCFG=0xa2` and terminal channel state. Thus the user-visible stall is
fixed, but the underlying deferred-interrupt defect remains open.

Two complete attempts to remove the ISR-to-task global gate were rejected on
real hardware:

1. Keeping `GLBLINTRMSK` continuously enabled, serializing pending snapshots
   with `Disable()/Enable()` and making SOF event-driven enumerated the devices
   but stopped boot progress after periodic mouse scheduling began.
2. Using `GLBLINTRMSK` only inside the ISR, with an acknowledgement readback
   and reopening it before `Cause()`, progressed farther but still stopped
   before `kms.library`/Wanderer once periodic traffic was active.

Those designs are investigation records, not current code. The driver was
returned to the exact known-working artifact after both failures. Future work
must start from that controlled baseline and isolate the periodic scheduling
interaction before changing the gate again. Instrumentation must remain until
the watchdog count is actually fixed on the Pi.

# Acceptance

- Move the pointer continuously while the Mesa demo runs: no stall.
- `[DWC2/Emu68:WD]` recovery count for a boot with a mouse and keyboard
  attached drops from thousands to approximately zero.
- An interrupt endpoint completes at its `bInterval`, not at the 10 ms
  watchdog cadence.

# Channel-release race

This remains unresolved in the accepted intermediate build. A proposed local
serialization protocol was tested only as part of the rejected no-gate build
and therefore is not present in current code. Its intended ordering was:

1. an ISR that entered first finishes and publishes its pending bits;
2. release atomically withdraws the channel from split pacing and both channel
   interrupt masks;
3. release clears the request and all published pending state;
4. only then can the channel be allocated and armed for a new request.

If revisited, this ordering must be validated independently. A generation
counter can help detect stale software state, but cannot replace the boundary:
hardware channel interrupts carry no generation identifying which software
owner armed the channel.

## If IRQ and task move to different CPUs

Any future local-serialization proof depends on both sides running on CPU 0.
`Disable()` prevents a local interrupt from interleaving with the task; it is
not an inter-CPU lock. Before changing either affinity, the protocol must gain:

- one inter-CPU lock shared by ISR publication (`channel[].pending` and
  `channels_pending`) and channel withdrawal/release;
- acquire/release memory ordering around the request owner, pending status and
  hardware-mask transition;
- an IRQ synchronization step that waits for a handler which already sampled
  `HAINT/HCINT` before the channel can be assigned to its next request;
- task-side `GINTMSK` read-modify-write protection using the same cross-CPU
  primitive rather than local `Disable()` alone.

A generation/epoch can remain useful as an assertion or stale-event detector,
but cannot replace that synchronization: DWC2 reports only a channel number
and HCINT bits, with no tag identifying which software owner armed it.
