# Bellatrix Host Reactor

## Status

Implemented and validated on Raspberry Pi 3B with the Musashi multicore
runtime, launcher, USB HID, HDMI audio and KS1.3/Battle Squadron.

**Target architecture: Core 0 is Control.** The intended design has Core 0 as
the control plane/host reactor, exactly as described in this document. The
table below shows the *current, temporary* stabilization arrangement instead:
Emu68's integration is being stabilized with the CPU parked on Core 0 (its
native JIT/vector/IRQ environment), purely to reduce the number of moving
variables while that work is in progress — see
`AI_context/issues/ISSUE-0058.md`. That is scaffolding, not a redesign: once
Emu68's integration is stable, the CPU is expected to move off Core 0 and
Core 0 reverts to owning control/host-reactor duties, matching this
document's target. Until then, the host reactor described here runs on
Core 3 instead. See
[`multicore_topology.md`](../AI_context/consolidated/multicore_topology.md)
for the current role→core map.

## Active core topology (current stabilization placement)

| Core | Role | Responsibility |
| --- | --- | --- |
| 0 | CPU *(temporary)* | boot, selected CPU backend (Emu68 or Musashi), physical IRQ ingress — target is to vacate this back to Control once Emu68 is stable |
| 1 | Auxiliary | parked; available for a measured future service or acceleration job |
| 2 | Chipset | exclusive Rigel ownership |
| 3 | Host reactor *(temporary location)* | physical host I/O — USB, Bluetooth, miniUART, presentation/timeline; target home is Core 0 |

Core numbers describe ownership, not interrupt priority. Physical ARM IRQs
and emulated Amiga IPL are separate contracts — see
[`irq_and_interrupts.md`](irq_and_interrupts.md).

## Core 3 ownership

Core 3 is the sole owner of CherryUSB/DWC2, Bluetooth host processing, the
physical UART and deferred console drain. Core 2 owns Paula and exchanges
serial bytes with Core 3 through SPSC queues. Core 3 does not access Rigel
state directly.

The launcher and runtime use the same service point:

```text
launcher (cooperative) ─┐
                       ├─> bellatrix_runtime_io_step() ─> Host Reactor
supervisor (1 kHz) ─────┘
```

`bellatrix_launcher_pump_usb()` remains as a compatibility callback, but no
longer calls `usb_host_step()` directly. There is no USB ownership handoff and
no `launcher_owns_usb` gate.

## Activation and dispatch

The reactor has an atomic pending bitmap for USB, Bluetooth, serial and
console work. Polling currently marks these bits at approximately 1 kHz. A
future IRQ handler will mark the same bits, acknowledge the physical source,
wake Core 3 and return. Driver stacks must not execute in the IRQ handler —
see [`irq_and_interrupts.md`](irq_and_interrupts.md) for the Bluetooth
top-half that already follows this rule.

Dispatch currently preserves this order:

```text
Bluetooth → USB → Paula/physical serial bridge → console
```

The 1 ms interval is an observable budget, not yet a preemptive boundary.
Drivers that exceed it complete normally and increment `budget_miss`.

### Concurrency semantics

The pending bitmap is a level-triggered request to inspect a service, not an
event counter. Producers set bits with a release `fetch_or`; Core 3 takes the
current work set with an acquire `exchange(..., 0)`. A source that still has
observable work after its bounded service must re-arm its bit. Periodic polling
also re-arms the poll set, so an event arriving around the exchange is observed
either in the current dispatch or the next one. Queues remain the source of
truth for payloads; the bitmap is only a wake/service hint.

The current fixed service order is deliberately simple. Before adding fairness
policy, instrumentation should expose per-service `service_runs`, `requeues`
and maximum pending age. Staying inside the aggregate budget is not sufficient
if a continuously active service can starve a later one.

Timing terminology is strict:

- activation period: approximately 1 ms;
- execution budget: an observable target measured by `budget_miss`;
- hard deadline: none; the reactor cannot currently preempt a driver.

## Measured baseline

Raspberry Pi 3B runtime after launcher:

```text
calls=4000 pending=00 budget_miss=0 avg=7us max=26us late_max=0us
usb=19us bt=0us serial=0us console=7us
```

The average reactor cost is about 0.7% of one core at 1 kHz. The observed
maximum is 2.6% of the 1 ms budget.

Launcher instrumentation exposed synchronous USB work:

```text
calls=16543 budget_miss=2 max=624899us late_max=650434us usb=624893us
```

These stalls happen before CPU and chipset workers launch and are reported
separately as `[CORE0-IO-LAUNCHER]`. Runtime statistics are reset afterward and
reported as `[CORE0-IO]`. (These log tags are historical from when the
reactor ran on Core 0; the launcher/pre-launch phase still runs on the boot
core before the multicore roles are assigned, which is why the tag persists.)

## Single-core behavior

The ownership model is identical in single-core builds. The activation source
changes:

```text
CPU backend → PAL_Runtime_Poll() → Host Reactor
```

Musashi returns cooperatively after execution blocks. Emu68 must preserve the
same bounded-return contract. IRQs can later reduce polling but do not change
the reactor API or ownership.

## Core 1 policy

Core 1 is not a generic I/O core. It is parked during stabilization and is
the candidate plane for a future measured, expensive, asynchronous job such
as RTG conversion or AHI mixing/resampling, once Emu68's own placement is no
longer in flux (see `multicore_topology.md`). Control, completion ordering
and physical device ownership remain on Core 3 and Core 0 respectively. No
service moves to Core 1 without measurements and an explicit queue/job
contract.

The first Core 1 user must define an SPSC request/completion protocol with a
monotonic sequence number and explicit input/output ownership. Core 1 would
compute a result but never publish physical or emulated visible state
directly. Core 3 commits host-visible results; Core 2 commits chipset-visible
results. This keeps acceleration separate from device and machine ownership.

## Audio boundary

Paula time and physical HDMI consumption are different clocks. Core 2 remains
responsible for producing samples according to emulated time. The intended
physical boundary is an SPSC audio ring consumed under Core 3 ownership, where
DMA/HDMI progression, underrun accounting and buffer scheduling can eventually
live. Expensive mixing or resampling may become a Core 1 job only when
measured:

```text
Core 2 (Paula production) → SPSC ring → Core 3 (physical HDMI)
                                   ↘ Core 1 (optional DSP job) ↗
```

## Current limitations

- USB/MSC operations are still synchronous and can block for hundreds of
  milliseconds. This is acceptable during launcher boot, but runtime MSC must
  be measured and may require request/completion or a Core 1 worker.
- The budget detects overruns but cannot preempt a driver.
- Polling adds up to approximately 1 ms activation latency.
- HDMI audio progression is currently coupled to chipset advancement rather
  than fully scheduled by the Host Reactor.
- The reactor has no periodic liveness heartbeat since the Core 0→Core 3
  move; it only logs once at startup. See `AI_context/issues/ISSUE-0062.md`.
- The unified reactor requires a dedicated single-core hardware smoke before
  that configuration is promoted to the same validated baseline.

## Invariants

- Exactly one physical owner enters each host driver stack.
- Core 0 observes emulated IPL, never physical device IRQs beyond its own
  ingress role — see [`irq_and_interrupts.md`](irq_and_interrupts.md).
- Core 2 exclusively owns Rigel.
- Core 3 performs bounded, non-blocking work during runtime where possible.
- Heavy computation may move to Core 1; control-plane ownership does not.
- Polling and future IRQ activation must produce equivalent service semantics.
