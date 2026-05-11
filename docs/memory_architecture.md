# `docs/architecture/memory_architecture.md`

````md id="vpsdwo"
# Bellatrix — Memory Architecture and Emu68 Integration

## Purpose

Define the canonical memory architecture of Bellatrix.

This document defines:

- memory ownership
- memory semantics
- Emu68 integration
- MMU integration
- Chip RAM / Fast RAM behavior
- overlay behavior
- DMA-visible memory
- memory mapping rules
- memory subsystem responsibilities

This document is the source of truth for:

```text
how memory behaves architecturally
````

---

# 1. Architectural Principle

> Memory semantics belong to Bellatrix.
> Execution mappings belong to Emu68.

Bellatrix defines:

* ownership
* visibility
* overlay behavior
* DMA participation
* architectural semantics
* temporal meaning

Emu68 materializes:

* optimized mappings
* host virtual memory
* MMU pages
* direct access paths
* high-memory execution layouts

---

# 2. Historical Context

Bellatrix emerged around Emu68 rather than replacing it.

Emu68 already provides:

* optimized execution
* modern memory mappings
* large address spaces
* MMU-backed execution
* RTG-oriented layouts
* modern expansion-friendly memory organization

Bellatrix adds:

* architectural ownership
* coherent semantics
* temporal visibility
* runtime consistency
* DMA-aware behavior
* compatibility structure

---

# 3. Historical Problem

Initial Bellatrix architecture used:

* Chip RAM only
* raw direct accesses
* trap-based handling
* bus-centric decode

Result:

* AROS stalled during early boot
* memory probing failed
* address space coherence was broken

---

# 4. Key Insight

AROS behaves like a modern operating system.

AROS expects:

* coherent memory map
* predictable address regions
* stable RAM visibility
* valid contiguous memory
* expansion-friendly layouts

AROS does NOT expect:

* trap-driven fake memory
* fragmented ad-hoc mappings
* inconsistent decode logic

---

# 5. Canonical Memory Model

Current architecture:

```text id="k1jlwm"
CPU / DMA / Chipset
         ↓
 bellatrix_mem_*()
         ↓
 memory_map_decode()
         ↓
 region handler
```

---

# 6. Memory Subsystem Components

| Component       | Responsibility   |
| --------------- | ---------------- |
| memory.c        | public API       |
| memory_map.c    | canonical decode |
| chip_ram.c      | Chip RAM         |
| fast_ram.c      | Fast RAM         |
| overlay.c       | ROM overlay      |
| autoconfig.c    | AutoConfig       |
| MMU integration | Emu68 mappings   |

---

# 7. Source of Truth

## Principle

> memory_map is the single source of truth for architectural memory layout.

No component may duplicate:

* decode rules
* ownership rules
* overlay rules
* region semantics

---

# 8. Current Memory Layout

## Current Layout

| Region       | Address Range     |
| ------------ | ----------------- |
| Chip RAM     | 0x000000–0x1FFFFF |
| Fast RAM     | 0x200000–0x9FFFFF |
| Custom Chips | 0xDFF000–0xDFFFFF |
| ROM          | 0xF80000–0xFFFFFF |

---

## Reserved Regions

| Region            | Purpose                                |
| ----------------- | -------------------------------------- |
| 0xE80000–0xEFFFFF | AutoConfig                             |
| >0x10000000       | extended memory / RTG / future runtime |

---

# 9. Chip RAM

## Characteristics

Chip RAM:

* is DMA-visible
* participates in chipset timing
* participates in raster-visible behavior
* belongs to chipset temporal domain

---

## Ownership

Chip RAM is observable by:

* CPU
* DMA clients
* Agnus
* Copper
* Blitter
* Bitplanes

---

# 10. Fast RAM

## Characteristics

Fast RAM:

* is CPU-visible
* is NOT chipset DMA-visible
* bypasses chipset DMA contention
* exists primarily for CPU execution

---

## Important Rule

> Chip RAM and Fast RAM share basic read/write semantics,
> but belong to different temporal domains.

---

# 11. Overlay Behavior

## Amiga Overlay Rule

At address 0x000000:

### Reads

```text id="j1jlwm"
overlay ON  → ROM
overlay OFF → Chip RAM
```

---

### Writes

```text id="i1jlwm"
ALWAYS → Chip RAM
```

Overlay only affects READ visibility.

---

# 12. Endianness

## Principle

All Bellatrix memory is BIG-ENDIAN.

Examples:

```c id="h1jlwm"
read16 = (hi << 8) | lo
read32 = (b0 << 24) | ...
```

---

# 13. CPU Access Model

## CPU Access Path

CPU accesses MUST use:

```text id="g1jlwm"
bellatrix_mem_read*
bellatrix_mem_write*
```

These paths define:

* architectural semantics
* region ownership
* overlay visibility
* memory behavior

---

# 14. DMA / Chipset Access Model

## DMA Access Path

DMA/chipset accesses MUST use:

```text id="f1jlwm"
bellatrix_chip_read*
bellatrix_chip_write*
```

These paths preserve:

* DMA visibility
* chipset semantics
* temporal ownership
* synchronization rules

---

# 15. Forbidden Access Patterns

## Forbidden

Components MUST NOT:

* bypass memory subsystem semantics
* duplicate decode logic
* define ownership locally
* infer semantics from mappings alone

---

# 16. Nature of Emu68

Emu68 is:

```text id="e1jlwm"
execution backend
+
MMU/runtime layer
+
modern execution substrate
```

Characteristics:

* direct mappings
* host pointers
* memory linearization
* optimized execution
* low mediation
* throughput-oriented design

Emu68 already naturally supports:

* large address spaces
* RTG-friendly layouts
* modern RAM mappings
* expansion-oriented memory models

---

# 17. Emu68 MMU Model

Emu68 may use:

* direct mapped pages
* host virtual memory
* linear RAM mappings
* optimized page tables
* high memory regions

This is intentional and architecturally valid.

---

# 18. Critical Architectural Rule

> Direct mappings are allowed for performance.
> Semantics belong exclusively to Bellatrix.

Emu68 does NOT define:

* memory ownership
* DMA visibility
* overlay semantics
* timing semantics
* architectural meaning

---

# 19. Correct Integration Model

Correct architecture:

```text id="d1jlwm"
Bellatrix memory model
        ↓
Emu68 MMU mappings
        ↓
optimized execution
```

Incorrect architecture:

```text id="c1jlwm"
Emu68 mappings
        ↓
architectural meaning inferred afterward
```

---

# 20. Runtime Integration

Memory participates in runtime semantics.

This includes:

* DMA contention
* visibility
* synchronization
* causal ordering
* temporal ownership

Memory is NOT merely storage.

Memory participates in machine behavior.

---

# 21. DMA-Visible Memory

## Principle

DMA-visible memory belongs to chipset timing domain.

Examples:

* Chip RAM
* bitplane fetches
* Copper accesses
* Blitter accesses

---

## Important Consequence

Memory visibility is temporal.

Access visibility may depend on:

* DMA arbitration
* runtime state
* ownership
* synchronization
* future runtime policies

---

# 22. AutoConfig

## Reserved Region

```text id="b1jlwm"
0xE80000–0xEFFFFF
```

Future behavior:

* board descriptors
* dynamic configuration
* Zorro II devices
* expansion boards

---

# 23. Extended Address Spaces

Emu68 already operates naturally with:

* large memory layouts
* RTG-oriented mappings
* extended address spaces
* high-memory execution regions

Bellatrix therefore does NOT need to invent large-memory support from scratch.

Instead:

```text id="a1jlwm"
Bellatrix provides semantic coherence
over an already modern execution environment.
```

---

# 24. Future Runtime Direction

Future memory architecture may evolve toward:

```text id="z0jlwm"
memory domains
shared buffers
DMA-visible windows
zero-copy regions
capability memory
RTG framebuffers
AudioGraph buffers
```

while preserving classic compatibility.

---

# 25. Runtime Philosophy

Bellatrix does NOT define memory by:

```text id="y0jlwm"
whatever is directly mapped
```

Bellatrix defines:

```text id="x0jlwm"
architectural meaning
ownership
visibility
timing semantics
```

Emu68 only materializes optimized execution mappings.

---

# 26. Final Rule

> memory_map defines architecture.
> Emu68 materializes optimized execution mappings.

```
```
