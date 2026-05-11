# `docs/architecture/runtime_and_timing.md`

````md
# Bellatrix — Runtime, Timing and Temporal Architecture

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

# 6. Current Multicore Runtime

## Core 0 — Execution Runtime

Responsibilities:

* Emu68/JIT
* instruction execution
* bus access
* memory access
* host integration
* modern services

---

## Core 1 — Video/DMA Runtime

Responsibilities:

* Agnus
* Denise
* beam
* raster
* copper
* DMA
* bitplanes
* blitter
* scanout

---

## Core 2 — Paula Runtime

Responsibilities:

* audio Paula
* serial Paula
* disk Paula
* interrupt consolidation
* INTREQ
* INTENA
* IPL publication

---

## Core 3 — IO/CIA Runtime

Responsibilities:

* CIA A/B
* timers
* RTC
* keyboard protocol
* UART host integration
* classic physical IO

---

# 7. Temporal Flow

Current runtime flow:

```text
ticks
  ↓
Core 3 evolves IO/timers
Core 1 evolves raster/DMA
Core 2 consolidates streams/IRQ
  ↓
IPL derived
  ↓
CPU reacts
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
