# `docs/architecture/runtime_and_timing.md`

````md
# Bellatrix — Runtime, Timing and Temporal Architecture

> **Architectural rebaseline (2026-07-15):** the target architecture keeps
> Core 0 as the control plane/host reactor. Section 6 below instead describes
> the *current, temporary* stabilization placement
> (`AI_context/issues/ISSUE-0058.md`, `AI_context/consolidated/
> multicore_topology.md`): Core 0 = CPU/boot/IRQ ingress, Core 1 = auxiliary
> (parked), Core 2 = chipset (Rigel), Core 3 = host reactor. The CPU sits on
> Core 0 only to reduce stabilization variables for the Emu68 integration; it
> is expected to move off Core 0 once that integration is stable, at which
> point Core 0 reverts to Control and the host reactor is expected to move
> back with it. The host reactor and the `BELLATRIX_TIMELINE_MODE` policies
> below are both recent additions from this same rebaseline — treat them as
> actively evolving, not as long-settled architecture.

## Timeline policies

Multicore builds select `BELLATRIX_TIMELINE_MODE=cpu|realtime|hybrid`:

- `cpu` is the deterministic compatibility mode; Core 1 progress is the Core 2
  horizon.
- `realtime` makes Core 0 convert `CNTPCT` into PAL CCK and publish an
  autonomous horizon.
- `hybrid` clamps that realtime horizon to CPU progress plus the configured
  maximum backlog.

Core 0 is the only wall-clock policy owner. Core 2 consumes the atomic horizon
and subdivides it at observable Rigel deadlines and posted-write timestamps.
The horizon is not a periodic CPU/Core 2 rendezvous. In self-paced modes, MMIO
observes or stamps current chipset time; only ordering contacts serialize.

## Purpose

Define the canonical runtime and temporal model of Bellatrix.

This document defines:

- temporal ownership
- runtime evolution
- multicore execution model
- DMA timing
- interrupt propagation
- causal ordering
- bus synchronization
- timing granularity
- cross-core evolution

This document is the source of truth for:

```text
how the machine evolves over time
````

---

# 1. Fundamental Principle

> The observable behavior of the machine emerges from the coordinated evolution of hardware domains.

Consequences:

* CPU does not own time
* MMIO does not create time
* JIT execution does not define world state
* the chipset defines observable machine behavior
* runtime coordinates causality between domains

---

# 2. External Time Source

Bellatrix consumes an external progression source.

Possible sources:

* host physical timer
* deterministic harness
* internal scheduler
* bare-metal runtime clock

This produces:

```text
ticks
```

which are distributed across runtime domains.

---

# 3. Runtime Model

## 3.1 Core Principle

> Runtime distributes ticks.
> Domains interpret ticks according to ownership.

---

## 3.2 Temporal Domains

| Domain  | Responsibility                    |
| ------- | --------------------------------- |
| CIA     | timers, TOD, alarm                |
| Agnus   | raster, beam, DMA, VBL            |
| Paula   | audio streams, serial, interrupts |
| Denise  | visual composition                |
| CPU     | instruction execution             |
| Runtime | synchronization and propagation   |

---

# 4. Temporal Ownership

## CIA

CIA owns:

* timer evolution
* TOD evolution
* alarm generation
* timer underflow events

CIA does NOT own:

* IPL
* interrupt priority
* DMA

---

## Agnus

Agnus owns:

* raster progression
* beam position
* DMA arbitration
* copper scheduling
* blitter timing
* VBL generation

Agnus defines the primary observable visual timeline.

---

## Paula

Paula owns:

* INTREQ
* INTENA
* interrupt consolidation
* IPL derivation
* audio stream timing
* disk Paula timing
* serial Paula timing

Paula is the ONLY interrupt consolidator.

---

## Denise

Denise owns:

* bitplane composition
* sprite composition
* playfield composition
* scanout

Denise consumes visual timeline state from Agnus.

Denise is NOT a primary temporal source.

---

## CPU

CPU owns:

* instruction execution
* bus requests
* interrupt consumption

CPU does NOT define global machine timing.

---

# 5. Runtime Evolution

## Principle

> Time only advances inside domains.

Examples:

* CIA advances timers
* Agnus advances raster/DMA
* Paula evolves streams/interrupts
* Denise consumes visual state

Machine/runtime coordinate propagation only.

---

# 6. Current Multicore Runtime (temporary stabilization placement)

This section describes where things run *today*, while Emu68 integration is
being stabilized. It is not the target architecture — see the note at the
top of this document.

## Core 0 — CPU Runtime *(temporary)*

Responsibilities:

* boot (`bellatrix_init()`), including launching the chipset (Core 2) and
  host reactor (Core 3) roles
* Emu68 JIT or Musashi (whichever backend is selected)
* instruction execution, bus access, memory access, host integration
* physical ARM IRQ/FIQ ingress — see
  [`irq_and_interrupts.md`](irq_and_interrupts.md)

Emu68's native JIT/vector/IRQ environment stays co-located here only to
minimize stabilization variables. Once that integration is proven stable,
the CPU is expected to move off Core 0, and Core 0 reverts to owning Control
(host reactor) duties.

---

## Core 1 — Auxiliary Runtime (parked)

Responsibilities:

* parked until a measured service or future acceleration job justifies moving
  it here — see `host_reactor.md`'s "Core 1 policy"

---

## Core 2 — Chipset Runtime (Rigel)

Responsibilities:

* Full Rigel chipset domain:
  * CIA A/B (timers, TOD, keyboard protocol)
  * Agnus (beam, raster, DMA, copper, blitter)
  * Paula (audio, serial, disk, INTREQ/INTENA, IPL)
  * Denise (bitplanes, sprites, scanout)

---

## Core 3 — Host Reactor *(temporary location; target home is Core 0)*

Responsibilities:

* physical host-I/O ownership and event dispatch (USB, Bluetooth, miniUART,
  console)
* presentation/timeline drain
* supervisor heartbeat and future event/deadline arbitration

See [`host_reactor.md`](host_reactor.md) for the full ownership and
concurrency contract.

---

# 7. Temporal Flow

Current runtime flow:

```text
ticks
  ↓
Core 2 (Rigel) evolves full chipset:
  CIA timers / Agnus raster+DMA / Paula IRQ / Denise scanout
Core 0 Host Reactor evolves USB + Bluetooth + physical serial/console
  ↓
IPL derived (Paula → Rigel → bus)
  ↓
Core 1 CPU reacts
```

---

# 8. Interrupt Model

## Principle

> INTREQ and INTENA belong exclusively to Paula.

---

## Correct Flow

```text
CIA / Agnus
     ↓
  events
     ↓
Paula consolidates INTREQ
     ↓
INTENA applied
     ↓
IPL derived
     ↓
CPU receives IRQ
```

---

## Rules

* CIA never publishes IPL directly
* Agnus never consolidates IRQ
* Paula is the only interrupt consolidator

---

# 9. DMA Model

## Principle

> DMA is part of the temporal evolution of the machine.

---

## Ownership

| Function            | Owner       |
| ------------------- | ----------- |
| DMA request         | client      |
| DMA arbitration     | Agnus       |
| temporal visibility | runtime/bus |

---

## Rules

DMA is NOT an isolated operation.

DMA participates in:

* bus contention
* observable timing
* machine causality

CPU competes implicitly with DMA.

---

# 10. Copper

## Principle

> Copper belongs to Agnus temporal ownership.

Characteristics:

* synchronized to beam
* executes during frame progression
* modifies registers dynamically
* participates in visual timeline

---

# 11. Bus Model

## Principle

> The bus is a temporal synchronization protocol, not just address decoding.

---

## Flow

```text
CPU
 ↓
request
 ↓
runtime/bus
 ↓
target domain
 ↓
response
 ↓
CPU
```

---

## Responsibilities

Bus/runtime must guarantee:

* ordering
* synchronization
* wait states
* temporal consistency
* arbitration visibility

---

## Rule

> No access may ignore chipset state.

---

# 12. MMIO Rules

## Principle

> MMIO must not arbitrarily advance time.

---

## Allowed

MMIO MAY:

* change registers
* trigger edge events
* modify hardware lines
* schedule future temporal effects

---

## Forbidden

MMIO MUST NOT:

* advance raster directly
* evolve timers artificially
* run temporal loops
* bypass runtime synchronization

---

# 13. Timing Granularity

## Principle

> Runtime must not skip relevant hardware events.

Relevant events include:

* raster changes
* VBL
* DMA arbitration
* CIA timer transitions
* serial events
* disk synchronization

---

## Recommended Strategy

```c
while (delta > 0) {
    step = min(delta, GRANULARITY);
    runtime_step_domains(runtime, step);
    delta -= step;
}
```

---

# 14. Observable Correctness

The machine is correct when:

* CIA timers behave coherently
* raster/VBL are coherent
* DMA arbitration behaves correctly
* Paula consolidates IRQ correctly
* CPU reacts correctly to IPL
* visual output reflects coherent state

---

# 15. Runtime Synchronization

## Current State

Current runtime still relies partially on:

* ordering assumptions
* coarse synchronization
* shared locks

---

## Future Direction

Runtime will evolve toward:

```text
RuntimeMailbox
RuntimeEvent
RuntimeSync
LogicalClock
```

Goals:

* explicit causality
* safe parallelism
* reduced global locks
* domain synchronization
* timestamped events

---

# 16. Cross-Core Communication

## Types

### MMIO

Characteristics:

* synchronous
* ordered
* strongly consistent

---

### Events

Characteristics:

* asynchronous
* timestamped
* causality-aware

---

# 17. Runtime Philosophy

Bellatrix is NOT:

```text
a CPU-centric emulator
```

Bellatrix is:

```text
a distributed temporal runtime
with Amiga-compatible observable behavior
```

---

# 18. Final Rule

> Bellatrix does not simulate isolated registers.
> Bellatrix simulates emergent hardware behavior.

```
```
