# Emu68 Machine Boundary Architecture

## Standalone Machine Policies and Bellatrix Bus Integration

**Status:** Proposed Architectural Baseline  
**Scope:** Emu68 / Bellatrix integration  
**Historical reference:** Bellatrix legacy branch  
**Related:** `Bellatrix.md`, `Rigel_integration.md`, `Expansion.md`

---

# 1. Purpose

This document defines the boundary between Emu68 and Bellatrix.

The architecture distinguishes:

~~~text
Emu68
  │
  ├── PiStorm
  │
  └── Standalone
         │
         ├── generic bare-metal policy
         └── Bellatrix policy
~~~

Bellatrix does not need to become a third independent Emu68 execution environment.

It is a distinct machine policy for standalone Emu68.

The central rule is:

> **Emu68 provides execution, MMU, and fault mechanisms. Bellatrix defines the machine.**

---

# 2. Ownership

Emu68 owns generic mechanisms:

~~~text
Emu68
  ├── M68K execution / JIT
  ├── exception handling
  ├── fault reconstruction
  ├── MMU implementation
  ├── map/unmap interfaces
  ├── page tables
  └── TLB management
~~~

Bellatrix owns machine policy:

~~~text
Bellatrix
  ├── address-space definition
  ├── which ranges are directly mapped
  ├── which ranges intentionally trap
  ├── meaning of trapped accesses
  ├── Bellatrix Bus
  └── Bellatrix machine semantics
~~~

Bellatrix MUST NOT duplicate generic Emu68 mechanisms.

---

# 3. Use the Existing Emu68 MMU Interfaces

Bellatrix MUST use the existing Emu68 mapping/unmapping mechanisms wherever they are sufficient.

The intended relationship is:

~~~text
Bellatrix machine policy
        │
        ├── DIRECT
        │      │
        │      ▼
        │ existing Emu68 map interface
        │
        └── TRAPPED
               │
               ▼
          existing Emu68 unmap /
          no-map mechanism
~~~

Bellatrix MUST NOT:

- maintain independent page tables;
- implement a parallel MMU;
- duplicate Emu68 translation logic;
- manipulate translation structures directly when an existing Emu68 interface already provides the required operation.

The existing Emu68 interfaces must be inspected before adding new MMU APIs.

A generic Emu68 MMU change is justified only if the existing interface cannot express the Bellatrix machine policy.

---

# 4. Machine Address Policy

Bellatrix may classify M68K-visible addresses as:

~~~text
DIRECT
    normal memory

TRAPPED
    valid machine hardware transaction

INVALID
    no defined machine semantics
~~~

For example:

~~~text
RAM
    → DIRECT

hardware MMIO
    → TRAPPED

true address hole
    → INVALID
~~~

`TRAPPED` does not mean architecturally invalid.

It means that the address intentionally has no direct ARM MMU translation so that the access reaches Bellatrix machine semantics.

---

# 5. MMU and Fault Policy Must Match

The Bellatrix memory policy and fault policy describe the same machine.

Therefore:

~~~text
Bellatrix machine definition
           │
     ┌─────┴─────┐
     │           │
     ▼           ▼
 MMU policy   fault policy
~~~

A hardware range intended to trap MUST NOT simultaneously have a direct mapping that bypasses the fault path.

Likewise, an intentionally trapped range MUST have defined Bellatrix handling.

This is a mandatory invariant.

---

# 6. Fault Boundary

Emu68 is responsible for reconstructing the faulting M68K access.

It should provide enough information to identify:

~~~text
address
direction
width
write value, if applicable
read-result path
~~~

After reconstruction:

~~~text
Emu68
   │
   │ reconstruct access
   ▼
Bellatrix machine hook
   │
   │ interpret access
   ▼
Bellatrix
~~~

The division is:

> **Emu68 determines what access happened. Bellatrix determines what the access means.**

Bellatrix MUST NOT duplicate ARM exception decoding.

Emu68 generic fault code MUST NOT interpret Bellatrix hardware.

---

# 7. Bellatrix Bus

Trapped Bellatrix hardware accesses enter the machine through one explicit boundary:

~~~text
M68K access
    │
    ▼
MMU
    │
    ▼
intentional fault
    │
    ▼
Emu68 fault reconstruction
    │
    ▼
Bellatrix machine hook
    │
    ▼
Bellatrix Bus
    │
    ▼
machine semantics
~~~

The Bellatrix Bus is a synchronous machine-transaction boundary.

It is not:

- a generic Emu68 bus;
- a provider framework;
- an MMU;
- an interrupt controller;
- a DMA mechanism;
- a timing mechanism.

---

# 8. Minimal Transaction

The Bellatrix Bus must preserve the original M68K transaction:

~~~text
address
width
read/write
value
ordering
side effects
~~~

For example:

~~~text
WRITE.W $8200,$DFF096

        │
        ▼

Bellatrix Bus

address   = $DFF096
width     = 16
direction = write
value     = $8200
~~~

Host ARM endianness must not alter the logical M68K transaction.

The exact C API should be defined only after inspecting the current and legacy Emu68 fault paths.

---

# 9. vectors.c

`vectors.c` should remain generic exception machinery.

Avoid:

~~~text
vectors.c
    ├── Bellatrix register decode
    ├── Rigel register decode
    └── Bellatrix device semantics
~~~

Prefer:

~~~text
vectors.c
    │
    ▼
fault reconstruction
    │
    ▼
machine hook
~~~

For Bellatrix:

~~~text
vectors.c
    │
    ▼
Bellatrix machine hook
    │
    ▼
Bellatrix Bus
~~~

Only the minimum target-selection mechanism required to reach the correct machine policy should exist in generic Emu68 code.

---

# 10. mmu.c

`mmu.c` remains the generic implementation of Emu68 memory translation.

Bellatrix determines the desired map but uses Emu68 to realize it:

~~~text
Bellatrix machine policy
        │
        ▼
existing Emu68 map/unmap API
        │
        ▼
mmu.c
        │
        ▼
page tables
~~~

Therefore:

> **Bellatrix defines the map; Emu68 implements the map.**

Target-specific address semantics MUST NOT be embedded into generic MMU internals when they can be expressed through the existing mapping interface.

---

# 11. PiStorm and Generic Standalone Preservation

Bellatrix-specific changes MUST NOT alter PiStorm behavior.

They also MUST NOT silently redefine existing standalone bare-metal behavior.

Conceptually:

~~~text
Emu68
  │
  ├── PiStorm
  │
  └── Standalone
         │
         ├── generic bare-metal
         └── Bellatrix
~~~

The exact build implementation does not need to reproduce this diagram literally.

The requirement is behavioral separation.

---

# 12. Legacy Bellatrix Reference

The legacy Bellatrix branch already demonstrated the useful dependency direction:

~~~text
Emu68 fault
     │
     ▼
Bellatrix hook
     │
     ▼
bellatrix_bus_access()
     │
     ▼
machine semantics
~~~

The legacy implementation should be inspected to recover:

- where the Emu68 hook occurred;
- how the M68K access was reconstructed;
- how reads returned values;
- how writes were forwarded;
- how the corresponding MMU ranges were configured.

The legacy branch is an implementation reference.

It is NOT a requirement to restore:

- the old VirtualBus;
- multicore request queues;
- epochs;
- WFE/SEV synchronization;
- old timing infrastructure;
- obsolete device abstractions.

Preserve the boundary, not the historical implementation around it.

---

# 13. Rigel

Rigel remains optional.

The correct dependency is:

~~~text
Emu68
   │
   ▼
Bellatrix machine hook
   │
   ▼
Bellatrix Bus
   │
   ▼
Rigel adapter
   │
   ▼
librigel
~~~

Emu68 MUST NOT know Rigel register semantics.

Bellatrix routes the transaction to the Rigel domain.

Rigel interprets the chipset register.

For example:

~~~text
Bellatrix:
    $DFFxxx → Rigel domain

Rigel:
    specific register semantics
~~~

The Bellatrix Bus must remain valid with Rigel disabled.

---

# 14. Timing, Interrupts, and DMA

The Bellatrix Bus handles CPU-visible hardware transactions only.

These remain separate:

~~~text
CPU MMIO
    → Bellatrix Bus

Rigel timing
    → Rigel temporal API

Rigel IPL
    → Bellatrix interrupt path

Rigel DMA
    → guest physical memory backend
~~~

DMA is not reverse Bellatrix Bus traffic.

The Bus is not an interrupt controller or timing authority.

---

# 15. Synchronous First

The initial Bellatrix Bus should be synchronous:

~~~text
Emu68 fault
     │
     ▼
Bellatrix Bus
     │
     ▼
machine component
     │
     ▼
result
     │
     ▼
Emu68 resumes
~~~

Do not initially restore the legacy multicore transport.

If cross-core execution is needed later, it may be implemented behind the same synchronous architectural boundary.

---

# 16. Implementation Sequence

The recommended sequence is:

~~~text
1. Inspect current Emu68 MMU interfaces
        │
        ▼
2. Identify existing map/unmap mechanisms
        │
        ▼
3. Inspect current fault path
        │
        ▼
4. Inspect legacy Bellatrix fault hook
        │
        ▼
5. Inspect legacy MMU configuration
        │
        ▼
6. Define Bellatrix machine policy
        │
        ▼
7. Express Bellatrix mappings using
   existing Emu68 map/unmap interfaces
        │
        ▼
8. Add the minimal Bellatrix machine hook
        │
        ▼
9. Add the synchronous Bellatrix Bus
        │
        ▼
10. Validate trapped accesses
        │
        ▼
11. Validate PiStorm unchanged
        │
        ▼
12. Validate generic standalone unchanged
        │
        ▼
13. Add optional Rigel routing
~~~

Do not introduce a new MMU abstraction before establishing that the existing Emu68 interfaces are insufficient.

---

# 17. Architectural Invariants

The following rules are normative.

### Emu68 owns mechanisms

Emu68 owns execution, exception reconstruction, MMU implementation, page tables, TLB handling, and mapping primitives.

### Bellatrix owns policy

Bellatrix defines its M68K-visible machine and address semantics.

### Reuse existing MMU interfaces

Bellatrix MUST use existing Emu68 map/unmap mechanisms where sufficient.

### No parallel MMU

Bellatrix MUST NOT maintain its own translation implementation or page tables.

### MMU/fault synchronization

Bellatrix mapping policy and fault semantics MUST describe the same machine.

### One machine boundary

Intentionally trapped Bellatrix CPU accesses cross one explicit Bellatrix machine boundary.

### Fault ownership

Emu68 reconstructs the access.

Bellatrix interprets it.

### Bus ownership

The Bellatrix Bus belongs to Bellatrix, not generic Emu68.

### PiStorm preservation

Bellatrix changes MUST NOT alter PiStorm semantics.

### Generic standalone preservation

Bellatrix changes MUST NOT silently redefine existing bare-metal standalone behavior.

### Rigel independence

Rigel is optional and MUST NOT leak into generic Emu68 exception handling.

### No provider framework requirement

No generic provider architecture is required.

### Separate mechanisms

Timing, interrupts, and DMA remain separate from the Bellatrix Bus.

---

# 18. Final Architecture

~~~text
                         M68K software
                              │
                              ▼
                         Emu68 Core
                              │
                    execution / faults
                              │
                              ▼
                         machine policy
                              │
                 ┌────────────┴────────────┐
                 │                         │
          generic standalone           Bellatrix
                                           │
                         ┌─────────────────┴─────────────────┐
                         │                                   │
                      DIRECT                              TRAPPED
                         │                                   │
                         ▼                                   ▼
              existing Emu68 map                    existing Emu68
                    interface                       unmap/no-map
                         │                                   │
                         ▼                                   ▼
                       MMU                              Data Abort
                                                             │
                                                             ▼
                                                   fault reconstruction
                                                             │
                                                             ▼
                                                   Bellatrix machine hook
                                                             │
                                                             ▼
                                                      Bellatrix Bus
                                                             │
                                              ┌──────────────┴──────────────┐
                                              │                             │
                                            native                    optional Rigel
                                                                            │
                                                                            ▼
                                                                       Rigel adapter
                                                                            │
                                                                            ▼
                                                                         librigel
~~~

---

# 19. Final Decision

Bellatrix is a standalone Emu68 machine policy, not necessarily a third independent execution target.

Bellatrix defines:

~~~text
what is mapped
what intentionally traps
what trapped accesses mean
~~~

Emu68 provides the mechanisms used to realize that policy.

In particular:

~~~text
Bellatrix
   │
   ▼
existing Emu68 map/unmap interfaces
   │
   ▼
existing Emu68 MMU implementation
~~~

Bellatrix MUST NOT introduce a parallel MMU implementation.

For trapped hardware accesses:

~~~text
M68K
   │
   ▼
intentional MMU trap
   │
   ▼
Emu68 fault reconstruction
   │
   ▼
Bellatrix machine hook
   │
   ▼
Bellatrix Bus
   │
   ▼
machine semantics
~~~

The legacy Bellatrix branch should be used to recover the proven fault hook and understand its relationship with the existing Emu68 MMU mechanisms.

The historical bus implementation itself does not need to be restored.

The architectural rule is:

> **Bellatrix defines the machine. Emu68 provides the mechanisms.**

And specifically:

> **Use the existing Emu68 MMU interfaces; do not create a second MMU architecture.**
