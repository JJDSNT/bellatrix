# `docs/architecture/system_architecture.md`

````md id="ayulnk"
# Bellatrix — System Architecture and Runtime Organization

> **Architectural rebaseline (2026-07-15):** the fixed Core0-supervisor /
> Core1-CPU topology below was superseded by ISSUE-0058. During stabilization,
> Emu68 remains on Core 0 with its native vector/fault/IRQ environment, Rigel
> owns Core 2, physical I/O runs on Core 3, and Core 1 is auxiliary. This
> placement is provisional, not the final product topology; integration
> contracts must remain independent of core number.

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

Bellatrix is being migrated toward a sparse ARM architecture. CPU accesses do
not target a closed `BellatrixMachine` box: direct memory remains mapped and
external regions reach their semantic owners directly. `BellatrixMachine` is
currently a transitional composition object, not the target CPU-facing ABI.

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

# 3. Sparse composition and transitional BellatrixMachine

## Current stabilization structure

```text id="w2jlwm"
BellatrixMachine
 ├── Runtime
 │    ├── Core 0 — CPU Runtime (provisional Emu68 native baseline)
│    ├── Core 1 — Auxiliary / parked during stabilization
│    ├── Core 2 — Chipset Runtime (Rigel: CIA+Agnus+Paula+Denise)
 │    └── Core 3 — Physical I/O reactor (USB/Bluetooth)
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

or:

```text
the universal CPU/MMIO gateway
```

While retained during migration, BellatrixMachine IS:

```text id="u2jlwm"
the explicit composition and coordination point
```

The target access path is sparse: classification dispatches directly to RAM,
CIA, custom-chip, Autoconfig or board owners. Coordination remains local to the
regions that require timing or cross-core synchronization.

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

## Core 0 — CPU Runtime (provisional)

Responsibilities:

* Emu68 JIT and native execution environment
* `VBAR_EL1`, `TPIDRRO_EL0`, Data Abort and fault-driven external bus
* native IRQ/STOP/timer assumptions retained until audited
* bounded physical-IRQ top half; no BTstack work in the vector

Core 0 is a conservative stabilization choice. It is not a permanent ownership
claim and must not leak into the CPU/bus ABI.

---

## Core 1 — Auxiliary Runtime

Responsibilities:

* parked in the current baseline
* available for a measured service or a future CPU placement
* any future migration requires equivalence with the Core 0 baseline

---

## Core 2 — Chipset Runtime (Rigel)

Responsibilities:

* Full Rigel chipset domain:
  * CIA A/B (timers, TOD, keyboard protocol)
  * Agnus (raster, beam, DMA, copper, blitter)
  * Paula (audio, serial, disk, INTREQ/INTENA, IPL)
  * Denise (bitplanes, sprites, scanout)

---

## Core 3 — Physical I/O Reactor (provisional)

Responsibilities:

* Bluetooth and USB deferred work outside exception context
* PL011 belongs to Bluetooth from boot
* AUX miniUART belongs to logging from boot
* may be reassigned only after the stabilized topology is measured

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
