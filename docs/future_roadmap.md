# `docs/architecture/future_roadmap.md`

````md id="kqjmrv"
# Bellatrix — Future Roadmap and Capability Runtime Direction

## Purpose

Define the long-term architectural direction of Bellatrix.

This document defines:

- future runtime direction
- capability-oriented evolution
- chipset deconstruction
- compatibility facades
- modern runtime services
- future multicore evolution
- modern APIs
- coexistence between classic and modern systems

This document is the source of truth for:

```text
where Bellatrix is evolving architecturally
````

---

# 1. Long-Term Vision

Bellatrix should NOT remain forever as:

```text
a literal emulation of classic Amiga chips
```

The long-term goal is:

```text
a modern capability-oriented runtime
with coherent Amiga compatibility
```

---

# 2. Fundamental Direction

Initial architecture:

```text id="j2jlwm"
software
    ↓
registers
    ↓
chips
    ↓
behavior
```

Future architecture:

```text id="i2jlwm"
classic software
        ↓
compatibility layer
        ↓
modern capability runtime

modern software
        ↓
modern runtime directly
```

---

# 3. Evolution Principle

> Initially, chips are the implementation.
> Eventually, chips become compatibility facades.

This means:

* compatibility remains
* implementation evolves
* runtime becomes capability-oriented

---

# 4. Why This Direction Exists

Bellatrix emerged around Emu68,
which already operates in a partially modern execution environment:

* large memory spaces
* MMU-backed execution
* RTG-oriented layouts
* optimized mappings
* modern host integration

Bellatrix therefore evolves toward:

```text id="h2jlwm"
architectural coherence over a modern execution substrate
```

rather than attempting to recreate a frozen 1980s machine internally.

---

# 5. Future Runtime Philosophy

Future Bellatrix becomes:

```text id="g2jlwm"
a distributed runtime architecture
with Amiga-compatible observable semantics
```

The system evolves from:

```text id="f2jlwm"
chip-centric architecture
```

toward:

```text id="e2jlwm"
capability-centric runtime architecture
```

---

# 6. Capability Runtime

Future runtime capabilities may include:

```text id="d2jlwm"
VideoTimeline
DisplayGraph
AudioGraph
InterruptRouter
TimerService
EventBus
StorageService
InputService
MemoryDomains
DMA Scheduler
```

---

# 7. Future Role of Classic Chips

Classic chips remain visible for compatibility:

* Agnus registers
* Paula registers
* CIA registers
* Denise registers

But internally they may become:

```text id="c2jlwm"
compatibility adapters
```

over modern runtime services.

---

# 8. Agnus Evolution

## Current Role

Agnus currently owns:

* raster
* beam
* DMA
* copper
* blitter
* bitplane timing

---

## Future Evolution

Agnus may evolve into:

```text id="b2jlwm"
RasterTimeline
DMAArbiter
DisplayScheduler
CopperVM
BlitterEngine
```

---

# 9. Denise Evolution

## Current Role

Denise currently owns:

* visual composition
* bitplanes
* sprites
* playfields
* scanout

---

## Future Evolution

Denise may evolve into:

```text id="a2jlwm"
RenderPipeline
LayerManager
Compositor
PaletteService
```

---

# 10. Paula Evolution

## Current Role

Paula currently owns:

* audio
* serial
* disk
* interrupt consolidation

---

## Future Evolution

Paula may evolve into:

```text id="z1jlwm"
AudioGraph
InterruptRouter
DiskStreamController
SerialService
```

---

# 11. CIA Evolution

## Current Role

CIA currently owns:

* timers
* TOD
* alarm
* classic IO

---

## Future Evolution

CIA may evolve into:

```text id="y1jlwm"
TimerService
InputPortService
EventSource
RTCService
```

---

# 12. Future Runtime Layers

The active multicore topology provides these planes:

```text
Core 0 = Control Plane / Host Reactor
Core 1 = CPU Plane
Core 2 = Chipset Plane
Core 3 = Acceleration Plane
```

Core 3 is assigned by measured jobs rather than by the broad category "I/O".
RTG conversion and AHI mixing/resampling are likely candidates; physical device
ownership, IRQ acknowledgement and completion ordering remain on Core 0.

## Layer 1 — Compatibility Runtime

Responsibilities:

* classic register semantics
* observable compatibility
* legacy software execution

---

## Layer 2 — Capability Runtime

Responsibilities:

* modern runtime services
* synchronization
* scheduling
* event routing
* high-level services

---

## Layer 3 — Native Modern APIs

Responsibilities:

* modern software interfaces
* modern rendering
* modern audio
* advanced storage
* networking
* future AI integration

---

# 13. Future Memory Evolution

Future memory architecture may include:

```text id="x1jlwm"
memory domains
shared buffers
zero-copy regions
RTG framebuffers
AudioGraph buffers
DMA-visible windows
capability memory
```

while preserving classic semantics.

---

# 14. Future Video Direction

Classic video compatibility remains:

* bitplanes
* sprites
* copper
* raster timing

But runtime may evolve toward:

```text id="w1jlwm"
RTG
compositing
layered rendering
modern display pipelines
```

---

# 15. Future Audio Direction

Classic Paula compatibility remains.

But internally Bellatrix may evolve toward:

```text id="v1jlwm"
AudioGraph
modern mixers
MIDI
DSP
synth engines
future voice systems
```

---

# 16. Future Input Direction

Classic compatibility remains:

* CIA keyboard
* joystick
* quadrature mouse

Modern runtime may evolve toward:

```text id="u1jlwm"
USB HID
Bluetooth
modern controllers
event translation
```

---

# 17. Future DMA Direction

DMA evolves from:

```text id="t1jlwm"
chip-specific behavior
```

toward:

```text id="s1jlwm"
runtime-managed bandwidth arbitration
```

with:

* multiple DMA clients
* scheduling policies
* runtime visibility
* temporal ownership

---

# 18. Future Interrupt Direction

Interrupts evolve from:

```text id="r1jlwm"
INTREQ / INTENA
```

toward:

```text id="q1jlwm"
InterruptRouter
EventBus
DeferredEvents
```

while preserving compatibility semantics.

---

# 19. Future Multicore Direction

Current cores still represent hardware domains.

Future cores may evolve toward capability runtimes.

---

## Target Direction

This table is the target architecture, not the current placement — see
`docs/runtime_and_timing.md` for why. Emu68 currently sits on Core 0
temporarily, to minimize stabilization variables during integration; the
plan is to vacate it back to Machine/Host once that integration is stable.

| Core | Target Runtime                              |
| ---- | -------------------------------------------- |
| 0    | Machine/Host — arbiter, parks in wfe after init |
| 1    | CPU Runtime (Emu68 JIT or Musashi)           |
| 2    | Chipset Runtime — full Rigel domain (CIA + Agnus + Paula + Denise) |
| 3    | IO Runtime (USB + Bluetooth)                 |

### Current (temporary) stabilization placement

| Core | Current Runtime                                   |
| ---- | -------------------------------------------------- |
| 0    | CPU Runtime (Emu68 JIT or Musashi) — temporary      |
| 1    | Auxiliary — parked                                  |
| 2    | Chipset Runtime — full Rigel domain                 |
| 3    | Host Reactor (USB + Bluetooth + miniUART + presentation) |

---

## Future Direction

| Core | Future Runtime             |
| ---- | -------------------------- |
| 0    | Execution Runtime          |
| 1    | Timeline + Video Runtime   |
| 2    | Audio + Event Runtime      |
| 3    | IO + Timer + Input Runtime |

---

# 20. Runtime Synchronization Future

Future runtime synchronization may evolve toward:

```text id="p1jlwm"
RuntimeMailbox
RuntimeEvent
RuntimeSync
LogicalClock
```

Goals:

* explicit causality
* timestamped events
* reduced global locks
* scalable multicore execution

---

# 21. Compatibility Principle

> Compatibility must be preserved even when implementation changes.

Bellatrix preserves:

* observable behavior
* architectural semantics
* compatibility contracts

NOT necessarily:

* literal internal implementation

---

# 22. Architectural Identity

Bellatrix is NOT:

```text id="o1jlwm"
a traditional emulator frozen around old chips
```

Bellatrix is:

```text id="n1jlwm"
a modern runtime architecture
preserving coherent Amiga semantics
```

---

# 23. Strategic Direction

Bellatrix evolves toward:

```text id="m1jlwm"
modern runtime
+
capability-oriented services
+
classic compatibility layer
```

rather than:

```text id="l1jlwm"
permanent literal chip emulation
```

---

# 24. Final Principle

> The future of Bellatrix is not replacing compatibility.
> The future of Bellatrix is abstracting implementation while preserving semantics.

```
```
