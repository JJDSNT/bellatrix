# Emu68 Machine Boundary Architecture

## Execution Environments, Machine Policies, MMU/Fault Synchronization, and Bellatrix Transactions

**Status:** Proposed Architectural Baseline  
**Scope:** Emu68 machine organization and Bellatrix integration  
**Related:** `Bellatrix.md`, `docs/Rigel_integration.md`, `docs/Expansion.md`  
**Historical reference:** Bellatrix legacy branch

---

# 1. Purpose

This document defines the architectural boundary between the Emu68 execution core and the machine environments in which it operates.

The architecture deliberately distinguishes two concepts:

~~~text
execution environment
        │
        └── how and where Emu68 runs

machine policy
        │
        └── what M68K-visible machine Emu68 exposes
~~~

These concepts MUST NOT be unnecessarily conflated.

At the broadest level, Emu68 currently operates in conceptually distinct execution environments such as:

~~~text
Emu68
  │
  ├── PiStorm
  │
  └── Standalone
~~~

Within the standalone environment, more than one machine policy may exist.

Conceptually:

~~~text
Standalone Emu68
      │
      └── machine policy
             │
             ├── generic bare-metal
             └── Bellatrix
~~~

The exact compile-time or runtime representation of these concepts is an implementation detail.

The architecture does NOT require PiStorm, generic bare-metal, and Bellatrix to be represented as three equivalent first-class targets.

The central requirement is instead:

> **The selected machine policy MUST define a coherent M68K-visible machine.**

In particular, it must consistently determine:

- which address ranges are directly mapped;
- which address ranges intentionally trap;
- which ranges have no machine semantics;
- what trapped accesses mean;
- which machine components own those semantics.

For Bellatrix specifically, intentionally trapped M68K hardware accesses enter the machine through one explicit synchronous transaction boundary:

~~~text
Emu68 fault reconstruction
          │
          ▼
Bellatrix machine hook
          │
          ▼
Bellatrix Bus
          │
          ▼
Bellatrix machine semantics
~~~

The central architectural rules are:

> **Emu68 owns execution mechanisms. The selected machine policy owns machine semantics.**

and:

> **MMU policy and fault semantics MUST describe the same machine.**

For Bellatrix:

> **Emu68 reconstructs the access. Bellatrix interprets the machine transaction.**

---

# 2. Architectural Model

The intended architecture is:

~~~text
                         Emu68 Core
                             │
                ┌────────────┼────────────┐
                │            │            │
            execution       MMU       exceptions
                │            │            │
                └────────────┼────────────┘
                             │
                  execution environment
                             │
                 ┌───────────┴───────────┐
                 │                       │
              PiStorm                Standalone
                                         │
                                         ▼
                                  machine policy
                                         │
                            ┌────────────┴────────────┐
                            │                         │
                    generic bare-metal            Bellatrix
                                                      │
                                                      ▼
                                               Bellatrix Bus
                                                      │
                                                      ▼
                                               machine semantics
~~~

This organization avoids inventing separate top-level targets when the underlying distinction is only a machine policy inside the same standalone execution environment.

---

# 3. Core Architectural Principle

Emu68 Core implements mechanisms.

The execution environment defines how Emu68 is hosted.

The machine policy defines what machine is visible to M68K software.

The distinction is:

~~~text
Emu68 Core
────────────────────────────────
M68K execution
JIT translation
exception entry
ARM context management
fault reconstruction
generic MMU primitives
page-table implementation
TLB management

Execution Environment
────────────────────────────────
host/platform integration
physical machine context
standalone versus PiStorm topology
environment-specific transport where required

Machine Policy
────────────────────────────────
M68K-visible address-space policy
direct mapping policy
intentional trap policy
meaning of trapped accesses
machine-specific semantics
~~~

These responsibilities may be represented by different files, build options, or internal structures.

The architecture does not mandate a particular source layout.

---

# 4. Execution Environments

The architecture distinguishes execution environments only where the hosting model is fundamentally different.

## 4.1 PiStorm

PiStorm operates in conjunction with an external Amiga machine environment.

Its behavior may include:

- physical Amiga bus access;
- PiStorm-specific address handling;
- PiStorm virtual hardware;
- physical hardware fallback;
- PiStorm-specific memory behavior;
- PiStorm-specific MMIO semantics.

Conceptually:

~~~text
PiStorm
   │
   ├── Emu68 execution
   ├── physical / virtual Amiga environment
   └── PiStorm-specific machine integration
~~~

PiStorm-specific behavior MUST remain isolated from generic standalone behavior.

---

## 4.2 Standalone

Standalone Emu68 operates without the PiStorm physical Amiga environment.

Conceptually:

~~~text
Standalone
    │
    ├── Emu68 execution
    ├── native host platform
    └── selected machine policy
~~~

The standalone environment may support more than one M68K-visible machine definition.

For example:

~~~text
Standalone
    │
    ├── generic bare-metal policy
    └── Bellatrix policy
~~~

This distinction is architectural.

Bellatrix does not need to become a completely separate Emu68 execution environment merely because its machine map differs from generic bare-metal.

---

# 5. Machine Policies

A machine policy defines the M68K-visible machine presented by a given execution environment.

Conceptually:

~~~text
machine policy
     │
     ├── address classification
     ├── direct mapping rules
     ├── intentional trap rules
     ├── trapped-access semantics
     └── machine-specific components
~~~

The important property is coherence.

A machine policy MUST define both:

~~~text
what maps directly
        +
what trapped accesses mean
~~~

These two decisions must never drift apart.

---

# 6. Generic Bare-Metal Policy

The generic standalone bare-metal policy represents the existing non-PiStorm Emu68 machine behavior.

Conceptually:

~~~text
Standalone
    │
    ▼
generic bare-metal policy
    │
    ├── directly mapped guest memory
    ├── native Raspberry Pi environment
    └── existing standalone semantics
~~~

Bellatrix MUST NOT silently redefine this policy.

Changes required only by Bellatrix belong to the Bellatrix machine policy.

---

# 7. Bellatrix Machine Policy

Bellatrix is a machine policy within the standalone Emu68 execution environment.

Conceptually:

~~~text
Standalone Emu68
       │
       ▼
Bellatrix machine policy
       │
       ├── Bellatrix memory model
       ├── Bellatrix native hardware
       ├── Bellatrix interrupt architecture
       ├── Bellatrix MMU policy
       ├── Bellatrix trapped-access policy
       ├── Bellatrix Bus
       └── optional compatibility components
~~~

Bellatrix owns its M68K-visible machine semantics.

It does not require PiStorm semantics.

It must not redefine generic bare-metal behavior.

---

# 8. Machine Policy as the Aggregating Abstraction

Within the standalone environment, the machine policy is the aggregation point for the machine definition.

For Bellatrix:

~~~text
Bellatrix machine policy
      │
      ├── memory semantics
      ├── fault semantics
      ├── native platform semantics
      ├── Bellatrix Bus
      └── optional compatibility
~~~

The architecture should not be reorganized around a speculative generic provider framework.

The Bellatrix Bus is subordinate to the Bellatrix machine policy.

---

# 9. Address Classification

A machine policy may conceptually classify M68K-visible addresses into three categories:

~~~text
DIRECT
    ordinary memory represented by direct MMU translation

TRAPPED
    architecturally valid machine transaction
    intentionally lacking direct translation

INVALID
    no defined machine semantics
~~~

For example:

~~~text
guest RAM
    → DIRECT

hardware MMIO
    → TRAPPED

true address hole
    → INVALID
~~~

The selected machine policy owns this classification.

Generic Emu68 machinery must not independently invent it.

---

# 10. MMU/Fault Synchronization

For every machine policy:

~~~text
MMU policy
    │
    │ MUST MATCH
    ▼
fault semantics
~~~

The following is invalid:

~~~text
MMU:
hardware address → directly mapped RAM

Fault semantics:
same address → hardware transaction
~~~

because the hardware path will never see the access.

The inverse is also invalid:

~~~text
MMU:
address → intentionally trapped

Fault semantics:
undefined
~~~

because an intentional machine transaction would fall into an undefined exception path.

The machine policy must define both sides together.

---

# 11. Shared Machine Selection

MMU setup and fault handling MUST use the same selected machine policy.

Conceptually:

~~~text
              selected machine policy
                       │
             ┌─────────┴─────────┐
             │                   │
             ▼                   ▼
        MMU decisions       fault semantics
             │                   │
             └─────────┬─────────┘
                       │
                       ▼
               same machine model
~~~

Independent build-time conditions capable of making these disagree are architecturally invalid.

---

# 12. Fault Reconstruction Boundary

Generic Emu68 exception machinery should reconstruct enough information to describe the trapped M68K access.

Conceptually:

~~~text
Data Abort
    │
    ▼
Emu68 exception machinery
    │
    ├── faulting address
    ├── read/write direction
    ├── access width
    ├── write value where applicable
    ├── read-result return mechanism
    └── required execution context
    │
    ▼
machine-policy boundary
~~~

The ownership rule is:

~~~text
Emu68:
    determine what access occurred

Machine policy:
    determine what that access means
~~~

---

# 13. Standalone Machine Flow

For standalone execution:

~~~text
M68K execution
      │
      ▼
Emu68
      │
      ▼
selected standalone machine policy
      │
      ├── DIRECT
      │      │
      │      ▼
      │   normal memory
      │
      └── TRAPPED
             │
             ▼
         Data Abort
             │
             ▼
      Emu68 reconstruction
             │
             ▼
      machine-policy hook
~~~

If the selected policy is generic bare-metal, existing standalone behavior applies.

If the selected policy is Bellatrix, the access enters Bellatrix.

---

# 14. Bellatrix Machine Boundary

For Bellatrix:

~~~text
M68K access
    │
    ▼
Bellatrix machine policy
    │
    ├── DIRECT
    │      │
    │      ▼
    │   normal access
    │
    └── TRAPPED
           │
           ▼
       Data Abort
           │
           ▼
    Emu68 reconstruction
           │
           ▼
    Bellatrix machine hook
           │
           ▼
    Bellatrix machine
~~~

This hook is the boundary between generic Emu68 mechanism and Bellatrix semantics.

---

# 15. Bellatrix Bus

Within Bellatrix, trapped M68K-visible machine transactions enter through the Bellatrix Bus.

Conceptually:

~~~text
Emu68 fault reconstruction
          │
          ▼
Bellatrix machine hook
          │
────────────────────────────────
      Emu68 / Bellatrix
          boundary
────────────────────────────────
          │
          ▼
Bellatrix Bus
          │
          ▼
machine semantics
~~~

The Bellatrix Bus is:

> a synchronous Bellatrix machine transaction boundary for intentionally trapped M68K-visible hardware accesses.

It is not:

- a generic Emu68 bus;
- an execution environment;
- a provider framework;
- an interrupt controller;
- a DMA transport;
- a timing authority.

---

# 16. Machine Hook Versus Bellatrix Bus

The Bellatrix machine hook and Bellatrix Bus are conceptually distinct.

The hook belongs to Emu68/Bellatrix integration:

~~~text
Emu68
   │
   ▼
Bellatrix machine hook
~~~

The Bus belongs inside Bellatrix:

~~~text
Bellatrix
   │
   ▼
Bellatrix Bus
   │
   ▼
machine semantics
~~~

Conceptually:

~~~text
Emu68
   │
   ▼
bellatrix_machine_access()
   │
────────────────────────────
 Emu68 / Bellatrix boundary
────────────────────────────
   │
   ▼
bellatrix_bus_access()
   │
   ▼
Bellatrix machine
~~~

The exact function names are illustrative.

---

# 17. Bellatrix Transaction Model

The initial Bellatrix Bus should be synchronous and minimal.

A transaction must preserve at least:

- M68K-visible address;
- access direction;
- access width;
- write value where applicable;
- read result where applicable;
- transaction result.

Conceptually:

~~~c
struct bellatrix_bus_transaction {
    uint32_t address;
    enum bellatrix_bus_direction direction;
    enum bellatrix_bus_width width;
    uint32_t value;
};
~~~

and:

~~~c
bellatrix_bus_result_t
bellatrix_bus_access(
    struct bellatrix_bus *bus,
    struct bellatrix_bus_transaction *transaction);
~~~

These definitions are illustrative and should only be finalized after inspection of the current and historical Emu68 fault paths.

---

# 18. Preserve the M68K Transaction

Bellatrix receives an M68K-visible machine transaction.

The boundary MUST preserve:

- address;
- width;
- direction;
- logical value;
- occurrence;
- ordering;
- side effects.

For example:

~~~text
WRITE.W $8200,$DFF096
          │
          ▼
        Emu68
          │
          ▼
Bellatrix machine hook
          │
          ▼
Bellatrix Bus

address   = $DFF096
width     = 16
direction = write
value     = $8200
~~~

Host ARM byte ordering must not redefine the logical M68K transaction.

---

# 19. vectors.c Responsibility

`vectors.c` should remain generic Emu68 exception machinery.

It should not accumulate Bellatrix machine decode.

Avoid:

~~~text
vectors.c
    ├── if Bellatrix hardware A...
    ├── if Bellatrix hardware B...
    └── ...
~~~

Prefer:

~~~text
vectors.c
    │
    ▼
fault reconstruction
    │
    ▼
selected machine-policy hook
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

---

# 20. mmu.c Responsibility

`mmu.c` remains generic MMU machinery.

Conceptually:

~~~text
mmu.c
    │
    ├── page-table implementation
    ├── mapping primitives
    ├── translation helpers
    ├── memory attributes
    └── TLB management
~~~

The selected machine policy determines what to map.

~~~text
machine policy
      │
      ▼
mapping decisions
      │
      ▼
generic mmu.c machinery
~~~

Machine-specific semantics must not become hidden inside generic MMU implementation.

---

# 21. Legacy Bellatrix Precedent

The Bellatrix legacy branch already demonstrated the essential dependency direction:

~~~text
M68K access
      │
      ▼
intentional translation fault
      │
      ▼
Emu68 fault machinery
      │
      ▼
Bellatrix-specific hook
      │
      ▼
bellatrix_bus_access()
      │
      ▼
machine semantics
~~~

The legacy branch should therefore be used to recover:

- the proven hook location;
- access reconstruction details;
- read-result handling;
- write-value extraction;
- relevant MMU behavior.

It should not be copied mechanically.

---

# 22. What Should Not Be Restored from Legacy

The new architecture does not require automatically restoring:

- historical VirtualBus;
- old multicore synchronization;
- request/response queues;
- epochs;
- WFE/SEV protocols;
- old timing synchronization;
- historical execution topology;
- obsolete device abstractions.

The architectural property worth preserving is simply:

~~~text
Emu68 fault
     │
     ▼
Bellatrix boundary
     │
     ▼
machine transaction
~~~

---

# 23. Rigel Integration

Rigel remains an optional Bellatrix component.

The desired relationship is:

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
public Rigel API
   │
   ▼
librigel
~~~

Generic Emu68 machinery MUST NOT know Rigel register semantics.

Bellatrix should only know enough to route a transaction to the Rigel compatibility domain.

Detailed chipset semantics belong to Rigel.

---

# 24. Timing, Interrupts, and DMA Remain Separate

The Bellatrix Bus owns only CPU-visible machine transaction routing.

It does not own:

~~~text
timing
interrupt generation/arbitration
DMA
~~~

For example:

~~~text
CPU MMIO
    → Bellatrix Bus

Rigel execution progress
    → Rigel temporal API

Rigel IPL
    → Bellatrix interrupt path

Rigel DMA
    → guest physical memory backend
~~~

These paths must remain architecturally distinct.

---

# 25. Synchronous First

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

Do not initially introduce:

- asynchronous completion;
- worker cores;
- queues;
- epochs;
- WFE/SEV synchronization;

unless correctness requires them.

Execution topology must not be encoded into the transaction contract.

---

# 26. Possible Source Organization

The exact filesystem layout is not normative.

Conceptually:

~~~text
Emu68
│
├── vectors.c
├── mmu.c
├── fault reconstruction
│
├── PiStorm environment
│
└── Standalone environment
        │
        └── machine policy
               │
               ├── generic bare-metal
               └── Bellatrix
                       │
                       ▼
                Bellatrix machine hook

Bellatrix
│
├── machine/
│   ├── memory.c
│   ├── bus.c
│   └── ...
│
└── optional compatibility
    └── Rigel adapter
~~~

Ownership and dependency direction matter more than filenames.

---

# 27. Refactoring Strategy

The refactoring should avoid introducing artificial top-level targets.

Recommended sequence:

~~~text
Phase 1
    │
    ▼
identify current PiStorm-specific
and standalone-specific behavior
    │
    ▼
Phase 2
    │
    ▼
separate execution-environment policy
from generic Emu68 mechanisms
    │
    ▼
Phase 3
    │
    ▼
preserve PiStorm behavior unchanged
    │
    ▼
Phase 4
    │
    ▼
preserve existing standalone behavior
as generic bare-metal machine policy
    │
    ▼
Phase 5
    │
    ▼
introduce Bellatrix as a standalone
machine policy
    │
    ▼
Phase 6
    │
    ▼
recover the proven legacy
Bellatrix fault hook
    │
    ▼
Phase 7
    │
    ▼
introduce minimal synchronous
Bellatrix Bus
    │
    ▼
Phase 8
    │
    ▼
synchronize Bellatrix MMU
and trapped-access semantics
    │
    ▼
Phase 9
    │
    ▼
validate PiStorm unchanged
    │
    ▼
Phase 10
    │
    ▼
validate generic standalone unchanged
    │
    ▼
Phase 11
    │
    ▼
integrate additional Bellatrix
machine domains
~~~

---

# 28. Architectural Invariants

The following rules are normative.

## Core Ownership

Emu68 Core owns generic execution, MMU mechanisms, exception mechanisms, and fault reconstruction.

## Execution Environment Ownership

The execution environment owns fundamentally different hosting behavior such as PiStorm versus standalone operation.

## Machine Policy Ownership

The selected machine policy owns the M68K-visible machine definition.

## No Mandatory Three-Target Model

The architecture does NOT require PiStorm, generic bare-metal, and Bellatrix to be represented as three equivalent Emu68 targets.

## Standalone Policies

Generic bare-metal and Bellatrix may exist as separate machine policies within the same standalone execution environment.

## MMU/Fault Synchronization

The selected machine policy's mapping decisions and fault semantics MUST describe the same machine.

## Bellatrix Ownership

Bellatrix owns Bellatrix-specific machine semantics.

## Bellatrix Bus Ownership

The Bellatrix Bus belongs to Bellatrix, not to generic Emu68.

## Transaction Reconstruction

Emu68 reconstructs trapped M68K accesses.

Bellatrix interprets their machine meaning.

## PiStorm Isolation

PiStorm-specific behavior MUST NOT become generic standalone semantics.

## Bare-Metal Preservation

Bellatrix MUST NOT silently redefine generic bare-metal standalone behavior.

## Rigel Independence

Rigel remains optional and independent of Emu68 exception internals.

## No Provider Requirement

No generic provider framework is required.

---

# 29. Review Checklist

Future changes should answer:

1. Is this generic Emu68 mechanism, execution-environment behavior, or machine-policy behavior?
2. Does this genuinely require a distinct execution environment?
3. Could this instead be represented as a machine policy within standalone?
4. Is Bellatrix being unnecessarily promoted into a completely separate top-level target?
5. Does MMU setup use the same machine selection as fault handling?
6. Can a trapped address accidentally become directly mapped?
7. Can a trapped address reach the machine hook without defined semantics?
8. Is PiStorm-specific behavior leaking into standalone?
9. Is Bellatrix-specific behavior leaking into generic bare-metal?
10. Is generic bare-metal behavior being assumed to define Bellatrix?
11. Is `vectors.c` interpreting Bellatrix hardware?
12. Is `mmu.c` independently defining Bellatrix semantics?
13. Does Emu68 reconstruct accesses without interpreting the machine?
14. Does Bellatrix interpret accesses without duplicating ARM fault decoding?
15. Is the Bellatrix Bus becoming a generic Emu68 abstraction unnecessarily?
16. Is execution topology leaking into the Bus contract?
17. Are timing, interrupts, or DMA being incorrectly assigned to the Bus?
18. Has the legacy hook been inspected before inventing a new mechanism?
19. Are PiStorm and existing standalone behavior preserved?
20. Is the resulting architecture simpler than introducing three artificial peer targets?

---

# 30. Final Architecture

The preferred architectural model is:

~~~text
                         M68K software
                              │
                              ▼
                         Emu68 Core
                              │
                    execution / MMU /
                 exception reconstruction
                              │
                              ▼
                   execution environment
                              │
                 ┌────────────┴────────────┐
                 │                         │
              PiStorm                 Standalone
                                           │
                                           ▼
                                    machine policy
                                           │
                              ┌────────────┴────────────┐
                              │                         │
                      generic bare-metal            Bellatrix
                                                        │
                                                        ▼
                                              Bellatrix machine hook
                                                        │
                                                        ▼
                                                  Bellatrix Bus
                                                        │
                                      ┌─────────────────┼─────────────────┐
                                      │                 │                 │
                                   native           optional           other
                                  semantics           Rigel            domains
                                                        │
                                                        ▼
                                                  Rigel adapter
                                                        │
                                                        ▼
                                                     librigel
~~~

The corresponding ownership model is:

~~~text
Emu68 Core
    owns generic execution mechanisms

Execution Environment
    owns fundamentally different hosting models

Machine Policy
    owns the M68K-visible machine definition

Bellatrix Machine Policy
    owns Bellatrix-specific mappings and fault semantics

Bellatrix Bus
    routes intentionally trapped Bellatrix machine transactions
~~~

---

# 31. Final Decision

The architecture should NOT require three equivalent Emu68 targets merely to represent:

~~~text
PiStorm
generic bare-metal
Bellatrix
~~~

Instead, it should distinguish:

~~~text
execution environment
        from
machine policy
~~~

A suitable model is:

~~~text
Emu68
  │
  ├── PiStorm
  │
  └── Standalone
         │
         └── machine policy
                ├── generic bare-metal
                └── Bellatrix
~~~

The exact implementation may differ if the current Emu68 build organization makes another representation simpler.

What is normative is the semantic separation, not the number of compile-time targets.

The central rules remain:

> **Emu68 reconstructs the access. The selected machine policy interprets it.**

> **MMU mapping decisions and fault semantics MUST always describe the same machine.**

For Bellatrix:

~~~text
Emu68
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
   ▼
Bellatrix machine semantics
~~~

Bellatrix therefore becomes a distinct machine definition without unnecessarily becoming a completely separate Emu68 execution environment.
