# `docs/architecture/system_architecture.md`

````md id="ayulnk"
# Bellatrix — System Architecture and Runtime Organization

## Purpose

Define the canonical structural architecture of Bellatrix.

This document defines:

- BellatrixMachine organization
- runtime organization
- domain ownership
- multicore structure
- component integration
- Bellatrix ↔ Emu68 relationship
- wiring philosophy
- runtime responsibilities

This document is the source of truth for:

```text
how the system is structurally organized
````

---

# 1. Architectural Identity

Bellatrix is NOT a standalone emulator replacing Emu68.

> Bellatrix emerges architecturally around Emu68.

Emu68 provides:

* 68k execution backend
* optimized runtime execution
* MMU/memory mappings
* host integration
* execution substrate

Bellatrix adds:

* chipset ownership
* temporal coherence
* runtime structure
* domain organization
* observable hardware behavior
* compatibility semantics

---

# 2. Architectural Principle

> Bellatrix is a runtime-organized machine architecture,
> not a collection of isolated emulated chips.

The machine is composed of:

* temporal domains
* ownership boundaries
* runtime coordination
* observable hardware semantics

---

# 3. BellatrixMachine

## Canonical Structure

```text id="w2jlwm"
BellatrixMachine
 ├── Runtime
 │    ├── Core 0 — Control Plane / Host Reactor (supervisor + physical IO)
│    ├── Core 1 — CPU Runtime (Emu68 JIT or Musashi)
│    ├── Core 2 — Chipset Runtime (Rigel: CIA+Agnus+Paula+Denise)
 │    └── Core 3 — Acceleration Plane (parked; future RTG/AHI jobs)
 │
 ├── CPU
 │    └── Emu68 backend
 │
 ├── CIA A
 ├── CIA B
 │
 ├── Paula
 │
 ├── Agnus
 │    ├── Copper
 │    ├── Blitter
 │    └── Bitplanes
 │
 └── Denise
```

---

# 4. Role of BellatrixMachine

BellatrixMachine is NOT:

```text id="v2jlwm"
the owner of time
```

BellatrixMachine IS:

```text id="u2jlwm"
the explicit composition and coordination point
```

---

## Responsibilities

BellatrixMachine owns:

* initialization
* reset
* runtime composition
* explicit wiring
* synchronization coordination
* global state publication

---

# 5. Architectural Layers

## Layer 1 — Execution Substrate

Provided primarily by Emu68.

Responsibilities:

* instruction execution
* MMU mappings
* optimized memory execution
* host interaction

---

## Layer 2 — Hardware Runtime

Provided by Bellatrix runtime.

Responsibilities:

* domain coordination
* DMA ownership
* interrupt consolidation
* timing semantics
* runtime synchronization

---

## Layer 3 — Compatibility Semantics

Provided by Bellatrix architecture.

Responsibilities:

* Amiga-compatible behavior
* observable hardware semantics
* chipset compatibility
* register semantics

---

# 6. Runtime Domains

Bellatrix is divided into explicit domains.

---

## CPU Domain

Responsibilities:

* instruction execution
* bus requests
* interrupt consumption

Current backend:

```text id="t2jlwm"
Emu68
```

---

## Agnus Domain

Responsibilities:

* raster
* beam
* DMA arbitration
* copper scheduling
* blitter timing
* VBL generation

Agnus owns the primary visual timeline.

---

## Paula Domain

Responsibilities:

* audio streams
* disk Paula
* serial Paula
* interrupt consolidation
* INTREQ
* INTENA
* IPL derivation

Paula is the ONLY interrupt consolidator.

---

## CIA Domain

Responsibilities:

* timers
* TOD
* alarm
* keyboard IO
* classic physical IO

CIA never publishes IPL directly.

---

## Denise Domain

Responsibilities:

* visual composition
* sprites
* bitplanes
* playfields
* scanout

Denise consumes visual timeline state.

---

# 7. Runtime Organization

## Core 0 — Control Plane / Host Reactor

Responsibilities:

* boot (`bellatrix_init()`)
* launching Core 1 and Core 2; Core 3 remains parked
* sole ownership of USB, Bluetooth, physical UART and console drain
* 1 kHz event/poll dispatch and runtime supervision
* future physical IRQ acknowledgement and pending-event publication

Launcher and runtime call the same Host Reactor service path. Core 0 never
routes physical ARM device IRQs into the Emu68 PiStorm INT6 path.

---

## Core 1 — CPU Runtime

Responsibilities:

* Emu68 JIT or Musashi (whichever backend is selected)
* CPU execution
* memory access
* bus dispatch
* host integration

---

## Core 2 — Chipset Runtime (Rigel)

Responsibilities:

* Full Rigel chipset domain:
  * CIA A/B (timers, TOD, keyboard protocol)
  * Agnus (raster, beam, DMA, copper, blitter)
  * Paula (audio, serial, disk, INTREQ/INTENA, IPL)
  * Denise (bitplanes, sprites, scanout)

---

## Core 3 — Acceleration Runtime (reserved)

Responsibilities:

* parked in the current baseline
* future RTG conversion/compositing jobs
* future AHI mixing/resampling jobs
* no physical-device ownership by default

---

# 8. Runtime Philosophy

## Principle

> Domains evolve independently.
> Runtime coordinates causality.

This allows:

* multicore evolution
* explicit ownership
* future capability runtimes
* coherent synchronization

---

# 9. Wiring Philosophy

## Principle

> Wiring must be explicit.

No component should depend on:

* hidden globals
* implicit ownership
* singleton assumptions
* duplicated integration logic

---

## Correct Wiring

Example:

```text id="s2jlwm"
machine
   ↓
agnus.memory = &machine.memory
```

Explicit ownership is always preferred.

---

# 10. Bus Philosophy

## Principle

> The bus is a synchronization protocol,
> not merely address decoding.

The bus coordinates:

* ordering
* visibility
* wait states
* synchronization
* temporal consistency

---

# 11. Memory Philosophy

## Principle

> Memory semantics belong to Bellatrix.

Emu68 may optimize execution mappings,
but architectural ownership remains in Bellatrix.

---

# 12. MMIO Philosophy

## Principle

> MMIO changes state.
> Runtime evolves time.

MMIO may:

* modify registers
* generate events
* alter hardware lines

MMIO must NOT:

* evolve temporal domains directly
* advance raster
* execute timing loops

---

# 13. DMA Philosophy

## Principle

> DMA is part of runtime causality.

DMA is NOT:

```text id="r2jlwm"
an isolated side effect
```

DMA participates in:

* bus arbitration
* temporal ownership
* observable behavior

---

# 14. Interrupt Philosophy

## Principle

> Interrupt ownership is centralized in Paula.

Correct flow:

```text id="q2jlwm"
CIA / Agnus
     ↓
  events
     ↓
Paula consolidates INTREQ
     ↓
IPL derived
     ↓
CPU reacts
```

---

# 15. Runtime Synchronization

## Current State

Current runtime still partially relies on:

* coarse synchronization
* global locks
* execution ordering assumptions

---

## Future Direction

Runtime will evolve toward:

```text id="p2jlwm"
RuntimeMailbox
RuntimeEvent
RuntimeSync
LogicalClock
```

Goals:

* explicit causality
* safe multicore operation
* timestamped events
* reduced global locking

---

# 16. Compatibility Philosophy

Bellatrix does NOT attempt to preserve:

```text id="o2jlwm"
literal hardware implementation
```

Bellatrix preserves:

```text id="n2jlwm"
observable behavior
architectural semantics
compatibility contracts
```

---

# 17. Architectural Evolution

Current architecture still models:

* Agnus
* Paula
* CIA
* Denise

as primary implementations.

Future architecture may evolve toward:

* capability runtimes
* compatibility facades
* modern services
* domain abstractions

without breaking compatibility.

---

# 18. Bellatrix vs Traditional Emulator

Bellatrix is NOT:

```text id="m2jlwm"
a CPU-centric emulator
```

Bellatrix is:

```text id="l2jlwm"
a runtime-organized hardware architecture
built around an optimized execution substrate
```

---

# 19. Runtime Identity

The runtime is responsible for:

* domain coordination
* synchronization
* causality
* ownership boundaries
* temporal coherence

The runtime is NOT merely:

```text id="k2jlwm"
a scheduler
```

It is part of machine semantics.

---

# 20. Final Principle

> Emu68 provides optimized execution.
> Bellatrix provides architectural coherence.

```
```
