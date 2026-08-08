Emu68 Target Boundary Architecture

Synchronizing MMU Mapping and Fault Semantics Across PiStorm, Bare-Metal, and Bellatrix

Status: Proposed Refactoring Baseline
Scope: Emu68 target organization and Bellatrix integration
Related: Bellatrix.md, docs/Rigel_integration.md, docs/Expansion.md

⸻

1. Purpose

This document defines the recommended architectural boundary between the Emu68 execution core and the machine environments in which it operates.

The central observation is that Emu68 currently supports conceptually distinct execution environments whose memory and fault semantics differ.

For the Bellatrix integration, these environments should be treated explicitly as three targets:

Emu68
  │
  ├── PiStorm
  │
  ├── Bare-metal non-PiStorm
  │
  └── Bellatrix

The target is the architectural unit that defines how the M68K-visible machine address space is realized.

In particular, a target must define consistently:

* which address ranges are directly mapped by the MMU;
* which address ranges are intentionally left without direct translation;
* what a fault in such a range means;
* how a trapped M68K-visible access is resolved;
* which machine semantics exist behind that access.

The primary invariant is:

Target MMU policy and target fault policy MUST describe the same machine.

⸻

2. Architectural Principle

Emu68 Core should implement execution mechanisms.

The selected target should define machine semantics.

Conceptually:

                    Emu68 Core
                        │
            ┌───────────┼───────────┐
            │           │           │
        execution      MMU       exceptions
            │           │           │
            └───────────┼───────────┘
                        │
                  target boundary
                        │
          ┌─────────────┼─────────────┐
          │             │             │
       PiStorm       Bare-metal    Bellatrix
                    non-PiStorm

This distinction prevents target-specific machine behavior from becoming embedded implicitly inside generic execution machinery.

⸻

3. The Three Targets

The target model reflects three distinct machine environments.

3.1 PiStorm

PiStorm executes M68K code while interacting with an external Amiga machine environment.

Its target semantics may include:

* physical Amiga bus access;
* PiStorm-specific address handling;
* Zorro/Autoconfig support;
* PiStorm virtual boards;
* fallback to physical Amiga hardware;
* PiStorm-specific MMIO behavior.

Conceptually:

PiStorm
   │
   ├── Emu68 translated execution
   ├── target-specific MMU policy
   ├── target-specific fault policy
   └── physical / virtual Amiga environment

⸻

3.2 Bare-Metal Non-PiStorm

The existing bare-metal Emu68 target operates without a physical Amiga bus.

Its machine model primarily consists of directly mapped memory and the native Raspberry Pi environment required by Emu68.

Conceptually:

Bare-metal non-PiStorm
        │
        ├── directly mapped guest memory
        ├── native Raspberry Pi environment
        └── no PiStorm physical-bus semantics

This target must remain independently supported.

Bellatrix MUST NOT silently redefine the existing bare-metal target.

⸻

3.3 Bellatrix

Bellatrix is a separate first-class Emu68 target.

It defines a native M68K execution platform whose machine semantics are controlled by Bellatrix rather than by PiStorm or the generic bare-metal environment.

Conceptually:

Bellatrix
    │
    ├── Bellatrix guest-memory model
    ├── Bellatrix native hardware
    ├── Bellatrix interrupt architecture
    ├── classic Expansion / Autoconfig
    └── optional compatibility components

Bellatrix therefore requires its own coherent MMU and fault policies.

⸻

4. MMU Policy and Fault Policy Are Complementary

A target’s memory architecture consists of two complementary classes of address ranges.

Target address space
        │
        ├── directly mapped
        │       │
        │       ▼
        │   normal memory access
        │
        └── intentionally trapped
                │
                ▼
            Data Abort
                │
                ▼
          target semantics

A directly mapped address is resolved through the MMU without invoking machine-level fault handling.

An intentionally trapped address has no direct MMU translation because access to that address represents a hardware transaction or another target-defined machine operation.

These are not independent decisions.

⸻

5. Fundamental Mapping Invariant

For every target:

MMU definition
      │
      │ MUST MATCH
      ▼
fault definition

The following state is invalid:

MMU:
$E80000 → ordinary mapped RAM
Fault policy:
$E80000 → Expansion

because the Expansion handler will never observe the access.

The inverse is also invalid:

MMU:
$E80000 → intentionally trapped
Fault policy:
no defined handling

because a deliberate machine address would fall into an undefined exception path.

The correct relationship is:

Target definition
       │
       ├── mapping decision
       │
       └── matching fault semantics

⸻

6. Explicitly Trapped Does Not Mean Architecturally Unmapped

A critical distinction must be preserved.

no MMU translation
        ≠
no machine semantics

For example, Bellatrix may deliberately leave:

$00E80000-$00E8FFFF

without direct MMU translation.

At the ARM/MMU level:

translation:
    absent

At the M68K machine level:

meaning:
    Expansion / Autoconfig

Therefore the correct terminology is preferably:

intentionally trapped address range

rather than simply:

unmapped address range

The latter can incorrectly suggest that the address has no architectural owner.

⸻

7. Legacy Bellatrix Precedent

The Bellatrix legacy architecture already demonstrated this general pattern.

Conceptually, its Emu68 path behaved as:

M68K JIT access
      │
      ▼
address without direct MMU mapping
      │
      ▼
Data Abort
      │
      ▼
Emu68 vectors / fault machinery
      │
      ▼
Bellatrix-specific boundary
      │
      ▼
bellatrix_bus_access()
      │
      ▼
Bellatrix machine decode

Classic hardware regions including:

* custom-chip space;
* CIA;
* Autoconfig;

were intentionally reached through this path rather than through ordinary memory mappings.

This precedent demonstrates that Bellatrix-specific machine semantics can be attached below Emu68 fault handling without modifying the M68K software using those addresses.

⸻

8. What Should Be Preserved from Legacy

The important legacy idea is:

A trapped Emu68 access crosses one Bellatrix-specific boundary and enters the Bellatrix machine definition.

That concept should be preserved.

The new architecture does not necessarily need to restore every implementation detail of the legacy Bellatrix Bus.

In particular, this document does not require restoring:

* the complete old bellatrix_bus_access() implementation;
* the old VirtualBus model;
* the old multicore synchronization architecture;
* the old machine decode implementation;
* obsolete device abstractions.

The reusable architectural principle is the boundary itself.

⸻

9. What Should Not Be Preserved Automatically

The existence of the old Bellatrix Bus does not imply that the new implementation requires a generic bus framework.

The following architecture is not required:

generic bus
    │
    ├── provider registration
    ├── generic device callbacks
    ├── dynamic ownership tables
    └── abstract provider lifecycle

Bellatrix is a defined machine target.

Its address semantics may be explicit and static.

The goal is not to turn Bellatrix into a generic emulator framework.

⸻

10. Target as the Aggregating Abstraction

The architectural aggregation point should be the target.

Not:

generic provider system
      │
      ├── Expansion
      ├── native hardware
      ├── compatibility hardware
      └── ...

But:

Emu68 target
    │
    ├── PiStorm
    │
    ├── Bare-metal
    │
    └── Bellatrix
            │
            ├── memory semantics
            ├── fault semantics
            ├── Expansion
            ├── native platform
            └── optional compatibility

This reflects the actual organization of the machine environments.

⸻

11. Core Versus Target Responsibilities

The recommended ownership boundary is:

Emu68 Core
────────────────────────────
M68K execution
JIT translation
exception entry
ARM context management
ESR/FAR handling
faulting instruction decode
MMU primitives
page-table implementation
TLB management
Target
────────────────────────────
machine address-space policy
which ranges are directly mapped
which ranges intentionally trap
meaning of trapped accesses
target-specific hardware semantics

The target should not reimplement generic Emu68 exception machinery.

The Emu68 core should not encode Bellatrix machine semantics.

⸻

12. Fault Reconstruction Boundary

The generic Emu68 exception path should reconstruct enough information to describe the trapped operation.

Conceptually:

Data Abort
    │
    ▼
Emu68 exception handling
    │
    ├── FAR
    ├── access direction
    ├── access width
    ├── value where applicable
    └── execution context required
    │
    ▼
target access boundary

The exact C representation does not need to be frozen by this document.

The important distinction is:

Emu68:
    determine what access occurred
Target:
    determine what that access means

⸻

13. Target Access Flow

The resulting flow should conceptually become:

M68K execution
      │
      ▼
Emu68 translated instruction
      │
      ▼
MMU
      │
     / \
    /   \
mapped   trapped
  │        │
  ▼        ▼
normal   Data Abort
memory      │
            ▼
        vectors.c
            │
            ▼
       access decode
            │
            ▼
      selected target
            │
     ┌──────┼──────┐
     │      │      │
 PiStorm BareMetal Bellatrix

⸻

14. Bellatrix Target Flow

For Bellatrix:

M68K access
    │
    ▼
Bellatrix MMU policy
    │
    ├── mapped memory
    │      │
    │      ▼
    │   direct access
    │
    └── trapped range
           │
           ▼
       Data Abort
           │
           ▼
       Emu68 decode
           │
           ▼
    Bellatrix target
           │
           ▼
    machine semantics

The Bellatrix target becomes the point where intentionally trapped addresses acquire meaning.

⸻

15. Expansion Example

Expansion is the clearest initial example.

Bellatrix defines:

$00E80000-$00E8FFFF

as classic Expansion/Autoconfig space.

The target memory policy therefore states:

Bellatrix:
$E80000-$E8FFFF
    → intentionally trapped
    → no direct RAM backing

The corresponding target fault policy states:

Bellatrix:
fault at $E80000-$E8FFFF
    → Expansion / Autoconfig access

Complete flow:

AROS expansion.library
        │
        ▼
M68K access $E80000
        │
        ▼
Bellatrix target MMU policy
        │
        ▼
no direct translation
        │
        ▼
Data Abort
        │
        ▼
Emu68 fault machinery
        │
        ▼
decoded M68K access
        │
        ▼
Bellatrix target semantics
        │
        ▼
Expansion / Autoconfig

⸻

16. AROS Remains Unaware of the Mechanism

AROS should not know that the Expansion access is implemented through an ARM Data Abort.

From the operating system perspective:

read/write $E80000
        │
        ▼
classic machine hardware

The underlying implementation may be:

MMU
  │
  ▼
Data Abort
  │
  ▼
Emu68
  │
  ▼
Bellatrix

This implementation mechanism must remain below the guest-visible architecture.

⸻

17. Relationship to AROS Expansion

This target boundary supports the intended AROS architecture:

AROS m68k-emu68
        │
        ▼
shared m68k-amiga
Expansion implementation
        │
        ▼
standard Autoconfig accesses
        │
        ▼
$E80000
        │
        ▼
Bellatrix target

No third Emu68-specific AROS Expansion algorithm is required.

The target supplies the machine semantics that allow the standard implementation to operate.

⸻

18. Relationship to Rigel

The target boundary also prevents unrelated machine domains from becoming incorrectly coupled.

For example, Bellatrix may eventually define:

Bellatrix target
      │
      ├── native platform semantics
      ├── Expansion semantics
      └── optional Rigel integration

Expansion remains independent from Rigel.

The target is the common machine environment containing both.

Conceptually:

                     Bellatrix target
                           │
              ┌────────────┼────────────┐
              │            │            │
           native       Expansion     Rigel
           machine      Autoconfig    optional
```
Rigel is not the aggregator.
Expansion is not the aggregator.
Bellatrix is.
---
# 19. Relationship to Native Hardware
The same principle applies to native Bellatrix hardware.
A native address may be:
* directly mapped;
* intentionally trapped;
* handled through another Bellatrix mechanism;
depending on its architecture.
The decision belongs to the Bellatrix target.
The MMU mechanism is subordinate to that machine definition.
---
# 20. Recommended `vectors.c` Refactoring
The purpose of refactoring `vectors.c` should not simply be to reduce its size.
The desired separation is:
~~~text
vectors.c
    │
    └── generic exception machinery
fault/access decode
    │
    └── reconstruct trapped access
target boundary
    │
    ├── PiStorm semantics
    ├── bare-metal semantics
    └── Bellatrix semantics

Target-specific hardware policy should gradually leave the generic exception machinery.

⸻

21. Recommended mmu.c Refactoring

Likewise, mmu.c should remain responsible for generic MMU implementation.

Conceptually:

mmu.c
    │
    ├── page-table creation
    ├── mapping primitives
    ├── translation helpers
    ├── memory attributes
    └── TLB management

The target determines what should be mapped.

Conceptually:

selected target
      │
      ▼
target memory policy
      │
      ▼
generic mmu.c machinery
      │
      ▼
actual translation tables

Target-specific memory semantics should not become hidden inside generic MMU code.

⸻

22. Shared Target Selection

MMU setup and fault handling MUST use the same target selection.

Conceptually:

                    build target
                        │
             ┌──────────┴──────────┐
             │                     │
             ▼                     ▼
      target MMU setup      target fault handling
             │                     │
             └──────────┬──────────┘
                        │
                        ▼
               same machine model

This is a normative requirement.

There must not be independent compile-time conditions capable of causing MMU policy and fault policy to disagree.

⸻

23. Possible Source Organization

The exact filesystem layout is not normative.

A possible minimal organization is:

src/aarch64/
│
├── vectors.c
├── mmu.c
├── fault_decode.c
│
└── targets/
    ├── pistorm.c
    ├── baremetal.c
    └── bellatrix.c

with corresponding headers where required.

Another valid organization may keep target implementation closer to the existing Emu68 layout.

The important property is conceptual ownership, not directory aesthetics.

⸻

24. Bellatrix Internal Decomposition

If Bellatrix target logic becomes large, it may later be decomposed internally.

For example:

targets/bellatrix/
│
├── target.c
├── memory.c
├── expansion.c
└── ...

This does not create a generic provider architecture.

The ownership remains:

Bellatrix target
      │
      └── internal implementation modules

The Bellatrix target remains the architectural aggregation point.

⸻

25. PiStorm Preservation

The refactoring MUST NOT require PiStorm to adopt Bellatrix semantics.

PiStorm may continue to implement behavior such as:

* physical Amiga bus fallback;
* PiStorm-specific Zorro handling;
* PiStorm boards;
* PiStorm-specific memory behavior.

The goal is to isolate this policy behind the PiStorm target boundary.

Conceptually:

same Emu68 core
      │
      ├── PiStorm machine definition
      └── Bellatrix machine definition

not:

Bellatrix replaces PiStorm architecture

⸻

26. Bare-Metal Preservation

The existing non-PiStorm bare-metal environment must likewise remain independently defined.

Bellatrix must not become the default meaning of “non-PiStorm”.

The three targets remain peers:

PiStorm
Bare-metal non-PiStorm
Bellatrix

This distinction is important for maintaining compatibility with upstream Emu68 behavior.

⸻

27. Refactoring Strategy

The refactoring should proceed without initially changing machine behavior.

Recommended sequence:

Phase 1
    │
    ▼
identify target-specific logic
currently embedded in vectors.c / mmu.c
    │
    ▼
Phase 2
    │
    ▼
introduce explicit target boundary
    │
    ▼
Phase 3
    │
    ▼
move PiStorm policy behind PiStorm target
without changing behavior
    │
    ▼
Phase 4
    │
    ▼
move bare-metal policy behind
bare-metal target
without changing behavior
    │
    ▼
Phase 5
    │
    ▼
move existing Bellatrix behavior behind
Bellatrix target
without changing behavior
    │
    ▼
Phase 6
    │
    ▼
validate all three targets
    │
    ▼
Phase 7
    │
    ▼
begin architectural changes such as
Bellatrix Expansion

Refactoring and semantic change should not be combined unnecessarily.

⸻

28. Behavioral Preservation Requirement

Before introducing new Bellatrix semantics:

PiStorm before refactor
        ==
PiStorm after refactor
Bare-metal before refactor
        ==
Bare-metal after refactor
Bellatrix before refactor
        ==
Bellatrix after refactor

where == means equivalent defined behavior.

Only after this is demonstrated should the new Bellatrix Expansion semantics be introduced.

⸻

29. Expansion Implementation Phase

Once the target boundary exists, Expansion becomes a Bellatrix target change rather than an Emu68-core change.

Conceptually:

Bellatrix target
      │
      ├── MMU:
      │      $E80000-$E8FFFF
      │      intentionally trapped
      │
      └── fault semantics:
             Expansion / Autoconfig

Then AROS can migrate from its transitional m68k-emu68 Expansion implementation to the shared Amiga implementation.

⸻

30. No Generic Provider Requirement

This architecture deliberately does not introduce a generic provider system.

No architectural requirement exists for concepts such as:

register_provider()
provider_ops
provider_context
provider_priority
dynamic provider matching

Such an abstraction should only be introduced later if multiple concrete requirements demonstrate that it is useful.

It must not be introduced merely to route a small number of known machine domains.

⸻

31. Architectural Invariants

The following rules are normative.

Core ownership

Emu68 Core owns execution, generic MMU machinery, and generic exception machinery.

Target ownership

The target owns machine-specific memory and fault semantics.

Three targets

PiStorm, bare-metal non-PiStorm, and Bellatrix are distinct first-class target environments.

MMU/fault synchronization

A target’s direct-mapping policy and fault-handling policy MUST describe the same address-space model.

Intentional traps

An address may be architecturally valid while intentionally lacking direct MMU translation.

Bellatrix ownership

Bellatrix is the aggregation point for Bellatrix machine semantics.

Expansion ownership

Expansion belongs to the Bellatrix machine definition when enabled.

It does not belong to Rigel.

No third AROS Expansion model

Bellatrix should expose hardware semantics that allow reuse of the standard AROS Amiga Expansion implementation.

PiStorm isolation

PiStorm-specific machine semantics MUST NOT become generic Emu68 semantics.

Bare-metal isolation

Bellatrix-specific machine semantics MUST NOT redefine the existing non-PiStorm bare-metal target.

No provider framework requirement

The architecture does not require a generic provider abstraction.

⸻

32. Review Checklist

Future changes to Emu68/Bellatrix target handling should answer:

1. Is this code generic Emu68 mechanism or target-specific machine policy?
2. If target-specific, which target owns it?
3. Does MMU setup use the same target definition as fault handling?
4. Can an intentionally trapped address accidentally become directly mapped?
5. Can an intentionally trapped address reach the fault path without defined semantics?
6. Is an architecturally valid hardware range being mislabeled as generic unmapped memory?
7. Is PiStorm-specific behavior leaking into generic Emu68 code?
8. Is Bellatrix-specific behavior leaking into the bare-metal target?
9. Is bare-metal behavior being assumed to define Bellatrix?
10. Is vectors.c interpreting device semantics that belong to a target?
11. Is mmu.c independently defining target-specific machine semantics?
12. Is a new generic abstraction being introduced without a concrete need?
13. Is Expansion being incorrectly attached to Rigel?
14. Could AROS use its standard classic hardware implementation against the resulting Bellatrix machine?
15. Does the change preserve behavior for targets not being modified?

If these questions cannot be answered cleanly, the target boundary should be reconsidered.

⸻

33. Target Architecture

The intended final relationship is:

                          Emu68 Core
                              │
                 ┌────────────┼────────────┐
                 │            │            │
             execution       MMU       exceptions
                 │            │            │
                 └────────────┼────────────┘
                              │
                       target boundary
                              │
              ┌───────────────┼───────────────┐
              │               │               │
           PiStorm         Bare-metal      Bellatrix
                           non-PiStorm
              │               │               │
         machine map      machine map      machine map
              │               │               │
         fault policy     fault policy     fault policy
                                              │
                                  ┌───────────┼───────────┐
                                  │           │           │
                               native     Expansion    optional
                               platform    Autoconfig  compatibility

The Bellatrix path is:

M68K execution
      │
      ▼
Emu68
      │
      ▼
Bellatrix target MMU
      │
      ├── direct mapping
      │      │
      │      ▼
      │   normal access
      │
      └── intentional trap
              │
              ▼
          Data Abort
              │
              ▼
        Emu68 fault decode
              │
              ▼
        Bellatrix target
              │
              ▼
       machine semantics

⸻

34. Relationship to the Legacy Bellatrix Bus

The legacy Bellatrix implementation can be understood as an earlier instance of the same architectural idea:

Emu68 fault
     │
     ▼
Bellatrix boundary
     │
     ▼
bellatrix_bus_access()
     │
     ▼
machine semantics

The new architecture should preserve:

one explicit Bellatrix boundary below Emu68 fault handling

without necessarily preserving:

the complete historical Bellatrix Bus implementation

The distinction is important.

The legacy branch provides evidence that the boundary works.

It does not dictate the internal architecture of the new Bellatrix machine.

⸻

35. Final Decision

The recommended direction is:

Refactor Emu68 around explicit target boundaries.

The three targets are:

PiStorm
Bare-metal non-PiStorm
Bellatrix

Each target owns a coherent pair:

memory-map policy
        +
fault semantics

The generic Emu68 core remains responsible for:

execution
MMU mechanisms
exception mechanisms
fault reconstruction

Bellatrix remains responsible for:

Bellatrix machine semantics
```
The legacy Bellatrix Bus demonstrates the correct direction of dependency:
~~~text
Emu68
   │
   ▼
Bellatrix boundary
   │
   ▼
Bellatrix machine

The new implementation should preserve that boundary while simplifying its internal architecture.

For Expansion specifically:

AROS
 │
 ▼
standard Amiga Expansion
 │
 ▼
$E80000
 │
 ▼
Bellatrix target MMU
 │
 ▼
intentional fault
 │
 ▼
Emu68 fault machinery
 │
 ▼
Bellatrix target semantics
 │
 ▼
Autoconfig

This provides a clean basis for removing the independent m68k-emu68 Expansion implementation and exposing the classic machine semantics that the standard AROS implementation already expects.
