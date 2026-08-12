# Rigel API Convergence Plan

## Refining the Existing Rigel Host Boundary for Bellatrix Integration

**Status:** Proposed API Boundary Refinement Baseline  
**Target:** Rigel API Version 1  
**Related specification:** `Bellatrix/docs/Rigel_integration.md`

---

# 1. Purpose

This document defines the recommended refinement of the existing Rigel public API so that the boundary between Rigel and its hosts is explicit, minimal, host-neutral, optimization-friendly, and suitable for long-term use by Bellatrix and the standalone harness.

The authority chain remains:

~~~text
Bellatrix.md
        │
        ▼
Rigel_integration.md
        │
        ▼
Rigel API Convergence Plan
        │
        ▼
Rigel public API
        │
        ▼
librigel implementation
~~~

The current Rigel implementation already contains the major architectural properties required by the Bellatrix/Rigel design.

The objective is therefore **not to redesign Rigel**.

The objective is:

> Preserve the existing Rigel execution and hardware boundary while refining the public host interface so that every CPU-visible classic hardware access implemented by Rigel remains correctly reachable from M68K software, without unnecessarily disabling existing Emu68 optimization mechanisms.

The fundamental compatibility requirement is:

> **No M68K software may fail merely because it accesses a CPU-visible classic hardware address implemented by Rigel that Bellatrix failed to expose or route correctly.**

The corresponding optimization requirement is:

> **Preserving complete Rigel hardware visibility must not require every access to traverse the generic MMU-fault and Bellatrix Bus path.**

This requirement concerns **semantic reachability**, not a mandatory physical dispatch path.

Bellatrix and Emu68 remain free to implement:

* caches;
* shadows;
* fast paths;
* direct paths;
* specialized handlers;
* pre-resolved providers;
* MMU interception;
* fault-based dispatch;
* generic Bus dispatch;

provided that these mechanisms preserve the observable Rigel hardware semantics.

In particular, existing Emu68 optimizations such as `INT_shadow` should be preserved where they avoid unnecessary fault handling and can remain coherent with Rigel ownership.

Therefore:

> **Complete hardware visibility is mandatory. A particular implementation path is not.**

And:

> **Optimization may bypass the generic fault path. It may not bypass the required hardware semantics.**

---

# 2. Core Compatibility Invariant

Let:

~~~text
R = CPU-visible classic hardware accesses
    implemented by Rigel

H = CPU-visible accesses that Bellatrix/Emu68
    can execute with correct Rigel semantics
~~~

The required invariant is:

~~~text
R ⊆ H
~~~

For every access supported by Rigel:

~~~text
access ∈ R
     │
     ▼
Bellatrix / Emu68
     │
     ├── shadow
     ├── cache
     ├── fast path
     ├── specialized path
     ├── MMU/fault path
     └── generic Bus path
              │
              ▼
     correct Rigel semantics
~~~

No address or transaction supported by Rigel may become inaccessible merely because it is absent from one particular optimization or dispatch mechanism.

Likewise, no optimization must be removed merely because it bypasses the generic path, provided it remains semantically correct.

---

# 3. Semantic Reachability, Not Mandatory Routing

The architecture MUST NOT require:

~~~text
every Rigel access
        │
        ▼
MMU fault
        │
        ▼
Bellatrix Bus
        │
        ▼
generic dispatcher
        │
        ▼
Rigel
~~~

That path is the generic fallback path.

It is not the only valid path.

The actual implementation may instead be:

~~~text
M68K access
     │
     ▼
Bellatrix / Emu68
     │
     ├── fast path ───────────────┐
     │                            │
     ├── cache ──────────────────┤
     │                            │
     ├── shadow ─────────────────┤
     │                            │
     ├── specialized handler ────┤
     │                            │
     └── generic fault / Bus ────┤
                                  │
                                  ▼
                         Rigel-equivalent
                       observable semantics
~~~

The optimization is valid when:

~~~text
observable(optimized path)
        =
observable(reference Rigel semantics)
~~~

The generic path therefore serves two roles:

~~~text
correctness fallback

semantic reference
~~~

It must not be interpreted as a mandatory runtime route for every transaction.

---

# 4. Rigel Defines Hardware Semantics

Rigel remains authoritative for the classic hardware semantics it implements.

Bellatrix must not independently redefine:

* custom register semantics;
* CIA semantics;
* Paula semantics;
* Agnus semantics;
* Denise semantics;
* `INTENA`;
* `INTREQ`;
* classic interrupt priority;
* DMA behavior;
* chipset timing.

Conceptually:

~~~text
Rigel
   │
   ├── defines classic hardware semantics
   │
   └── defines CPU-visible compatibility behavior
            │
            ▼
Bellatrix / Emu68
            │
            └── may optimize how those semantics
                are reached
~~~

The distinction is:

~~~text
SEMANTIC AUTHORITY
        │
        ▼
       Rigel


EXECUTION / OPTIMIZATION POLICY
        │
        ▼
Bellatrix / Emu68
~~~

---

# 5. Existing Emu68 Optimization Mechanisms

Emu68 already contains specialized handling and optimization mechanisms associated with classic Amiga hardware operation.

These mechanisms are not inherently incompatible with Rigel.

They should be treated as:

~~~text
Emu68 execution optimizations
~~~

rather than:

~~~text
independent definitions
of classic hardware semantics
~~~

Examples include conceptually:

~~~text
fast paths

specialized MMIO handling

cached routing/state

shadow state

INT_shadow

JIT optimizations

fault bypasses
~~~

Bellatrix integration should preserve useful Emu68 optimizations wherever they remain semantically correct.

The objective is **not** to force all classic hardware traffic through a slow generic dispatcher.

In particular:

> **An existing optimization that safely avoids an MMU fault is valuable and should not be removed merely to simplify the conceptual architecture.**

---

# 6. Emu68 `INT_shadow`

Emu68 contains `INT_shadow` handling associated with the classic interrupt registers:

~~~text
INTENA

INTREQ
~~~

This mechanism has direct performance relevance to Bellatrix.

Its important property is not merely that it mirrors interrupt state.

Its important property is that specialized handling may allow accesses to these registers to avoid the generic:

~~~text
MMU fault
    │
    ▼
fault reconstruction
    │
    ▼
Bellatrix Bus
    │
    ▼
Rigel MMIO
~~~

path.

Conceptually:

~~~text
M68K INTENA / INTREQ access
             │
             ▼
         Emu68 recognizes
       optimized interrupt path
             │
             ▼
          INT_shadow
             │
             ▼
      avoid generic fault path
~~~

This optimization should therefore be treated as potentially valuable and preserved unless profiling or correctness analysis proves otherwise.

However, Rigel remains authoritative for the classic interrupt semantics.

---

# 7. `INT_shadow` and `INT.IPL` Solve Different Problems

`INT_shadow` and `INT.IPL` must not be treated as alternatives.

They solve different problems.

~~~text
INT_shadow
    │
    ▼
optimize CPU access to
INTENA / INTREQ
and potentially avoid faults


Rigel IPL / Emu68 INT.IPL
    │
    ▼
deliver the already-resolved
classic interrupt level to the CPU
~~~

The desired architecture may therefore use both:

~~~text
CPU access INTENA / INTREQ
           │
           ▼
      Emu68 fast path
           │
       INT_shadow
           │
           ▼
   preserve Rigel semantics
           │
           ▼
          Rigel
           │
     INTENA / INTREQ
           │
           ▼
   priority resolution
           │
           ▼
        Rigel IPL
           │
           ▼
      Emu68 INT.IPL
           │
           ▼
          M68K
~~~

Thus:

> **`INT_shadow` is an access optimization. `INT.IPL` is an interrupt-delivery boundary.**

They are complementary.

---

# 8. Shadow Authority Rule

The fundamental rule for shadows is:

> **A shadow may accelerate access to Rigel-owned state, but it must not become an independent source of classic hardware truth.**

The preferred relationship is:

~~~text
                 authoritative
                    Rigel
                      │
                      ▼
                 INTENA/INTREQ
                      │
          ┌───────────┴───────────┐
          │                       │
          ▼                       ▼
   generic/reference        synchronized
        path                Emu68 shadow
                                  │
                                  ▼
                              fast path
~~~

The invalid architecture is:

~~~text
Rigel INTENA/INTREQ          Emu68 INT_shadow
        │                           │
        ▼                           ▼
   state machine A             state machine B
        │                           │
        └──────── disagreement ─────┘
~~~

There must not be two independently evolving interpretations of classic interrupt state.

---

# 9. Preserve `INT_shadow` as an Optimization

For Bellatrix, the recommended baseline is:

~~~text
KEEP:
    INT_shadow optimization

KEEP:
    Rigel ownership of INTENA

KEEP:
    Rigel ownership of INTREQ

KEEP:
    Rigel interrupt-source ownership

KEEP:
    Rigel priority resolution

USE:
    Rigel IPL → Emu68 INT.IPL
~~~

The objective is not:

~~~text
remove INT_shadow
because Rigel owns interrupts
~~~

The objective is:

~~~text
retain INT_shadow
because it may avoid faults

while

preventing INT_shadow
from becoming the authority
for classic interrupt state
~~~

This distinction is central.

---

# 10. `INT_shadow` Must Not Define Coverage

The existence or absence of an address in `INT_shadow` must never determine whether that address is CPU-visible.

This is forbidden:

~~~text
address represented by INT_shadow
        │
        ▼
visible

address not represented by INT_shadow
        │
        ▼
invisible
~~~

Instead:

~~~text
Rigel supports access?
        │
       / \
     yes  no
     │
     ▼
must remain reachable
     │
     ├── optimized shadow path
     │      if available
     │
     └── generic fallback
            otherwise
~~~

Therefore:

> **Shadow coverage may be incomplete. Hardware visibility may not be incomplete.**

---

# 11. Shadow Miss Must Fall Back Correctly

If an optimized Emu68 path does not recognize a Rigel-supported access, that must not make the access disappear.

Conceptually:

~~~text
M68K access
     │
     ▼
optimized handler available?
     │
    / \
  yes  no
   │    │
   ▼    ▼
fast   generic
path   fallback
   │      │
   └──┬───┘
      │
      ▼
correct Rigel semantics
~~~

A shadow or fast-path miss means only:

~~~text
this optimization did not handle
the transaction
~~~

It must not mean:

~~~text
hardware does not exist
~~~

For a Rigel-supported transaction, fallback must remain available.

---

# 12. Generic Fallback and Fast Path

The architecture should explicitly maintain both concepts:

~~~text
FAST PATH
    for performance

GENERIC FALLBACK
    for complete reachability
~~~

For example:

~~~text
                     M68K access
                         │
                         ▼
                  optimized path?
                      /     \
                    yes      no
                     │        │
                     ▼        ▼
                 INT_shadow  MMU fault
                     │        │
                     │        ▼
                     │   Bellatrix Bus
                     │        │
                     │        ▼
                     │   canonical MMIO
                     │        │
                     └────┬───┘
                          ▼
                   Rigel semantics
~~~

This preserves both:

~~~text
performance

and

complete compatibility
~~~

---

# 13. `INTENA` / `INTREQ` Special Case

`INTENA` and `INTREQ` directly influence classic interrupt state.

The intended ownership model is:

~~~text
M68K write/read
      │
      ▼
Emu68 / Bellatrix
      │
      ├── INT_shadow fast path
      │
      └── generic path
              │
              ▼
      Rigel interrupt semantics
              │
        INTENA / INTREQ
              │
              ▼
       priority resolution
              │
              ▼
          Rigel IPL
              │
              ▼
        Emu68 INT.IPL
~~~

Regardless of the physical path used, the resulting externally observable behavior must match Rigel semantics.

---

# 14. Optimized CPU Writes

CPU writes to `INTENA` or `INTREQ` may use the optimized Emu68 path.

For example:

~~~text
M68K write INTENA
       │
       ▼
Emu68 optimized handling
       │
       ├── avoid generic fault
       ├── maintain optimization state
       └── apply authoritative
           Rigel-visible semantics
                    │
                    ▼
                 Rigel
~~~

The optimization must not merely update an Emu68-local shadow while leaving Rigel unaware of a hardware-visible state transition.

The result must remain equivalent to the canonical Rigel transaction.

---

# 15. Rigel-Originated Interrupt Changes

Not all interrupt-state changes originate from CPU writes.

Classic devices may generate interrupt requests.

~~~text
Paula
CIA
Blitter
Copper
VBlank
other sources
      │
      ▼
     Rigel
      │
      ▼
    INTREQ
      │
      ▼
priority resolution
      │
      ▼
   Rigel IPL
~~~

This creates an important asymmetry:

~~~text
CPU → INTENA/INTREQ
    may use INT_shadow fast path

Rigel device → INTREQ
    originates inside Rigel
~~~

Therefore, any shadow state that is observable or required by the optimized CPU path must remain coherent with Rigel-originated changes.

The shadow cannot assume that interrupt state changes only occur through M68K register writes.

---

# 16. Shadow Synchronization Contract

If `INT_shadow` remains active in Bellatrix builds, its synchronization semantics must be explicit.

The implementation must establish:

~~~text
what state INT_shadow represents

who writes the shadow

when the shadow is updated

which accesses can be satisfied from it

whether writes can be handled entirely
through the optimized path

how authoritative Rigel state is updated

how Rigel-originated INTREQ changes
are reflected or invalidate the shadow

how reset initializes shadow state

whether asynchronous interrupt changes
invalidate cached assumptions

whether shadow state participates in
any CPU-facing read behavior
~~~

The critical invariant is:

~~~text
shadow-visible behavior
        │
        ▼
must never contradict
authoritative Rigel behavior
~~~

---

# 17. Shadow Synchronization Must Not Reintroduce the Fault

The purpose of preserving `INT_shadow` is partly to avoid generic fault overhead.

Therefore the synchronization design should not accidentally reduce the optimization to:

~~~text
INT_shadow hit
     │
     ▼
still trigger generic MMU fault
     │
     ▼
Bellatrix Bus
~~~

for every transaction.

The preferred goal is:

~~~text
M68K access
     │
     ▼
INT_shadow / specialized Emu68 path
     │
     ▼
minimal Rigel integration action
     │
     ▼
correct authoritative semantics
~~~

The exact synchronization mechanism is implementation-defined.

Possible designs may include:

* direct specialized Rigel entry;
* shared coherent state;
* narrow callback;
* deferred synchronization where semantically valid;
* derived state;
* explicit invalidation;
* another optimized integration mechanism.

The API should not prescribe one prematurely.

---

# 18. Prefer Derived or Invalidatable Shadows

Where practical, shadow state should behave as:

~~~text
derived / accelerated state
~~~

rather than:

~~~text
parallel hardware authority
~~~

Possible relationships include:

~~~text
Rigel authoritative state
          │
          ▼
      shadow refresh
          │
          ▼
      Emu68 shadow
          │
          ▼
       fast access
~~~

or:

~~~text
Rigel state change
       │
       ▼
invalidate shadow
       │
       ▼
next optimized access
refreshes required state
~~~

or another coherent optimization.

The exact implementation is not normative.

---

# 19. Rigel IPL Remains Authoritative

Bellatrix must consume Rigel's resolved compatibility-domain IPL.

~~~text
Rigel interrupt sources
        │
        ▼
INTREQ / INTENA
        │
        ▼
Rigel priority resolution
        │
        ▼
rigel_get_ipl()
        │
        ▼
Bellatrix IPL arbitration
        │
        ├── native IPL
        └── Rigel IPL
        │
        ▼
Emu68 INT.IPL
        │
        ▼
M68K
~~~

Bellatrix must not reconstruct the compatibility IPL from `INT_shadow`.

`INT_shadow` may accelerate register access.

It does not replace:

~~~text
Rigel interrupt resolution
~~~

or:

~~~text
Rigel IPL
~~~

Therefore:

> **Rigel IPL is the architectural classic interrupt result. `INT_shadow` is an optimization of register access.**

---

# 20. Canonical MMIO as Semantic Reference

Rigel should provide a canonical M68K-visible MMIO interface.

Conceptually:

~~~text
address
width
direction
value
     │
     ▼
canonical Rigel MMIO
     │
     ▼
classic hardware semantics
~~~

This interface defines the normal semantic reference.

However:

> **Canonical MMIO is not necessarily the physical path taken by every optimized access.**

For example:

~~~text
INTENA write
   │
   ├── generic path
   │      └── canonical MMIO
   │
   └── Emu68 INT_shadow fast path
          └── equivalent Rigel semantics
~~~

Both are valid when observable behavior is equivalent.

---

# 21. CPU-Visible Hardware Coverage

Rigel should provide an authoritative description sufficient for the host to know which CPU-visible classic hardware behavior must remain reachable.

Conceptually:

~~~c
struct rigel_mmio_region {
    uint32_t start;
    uint32_t end;
    uint32_t flags;
};
~~~

The exact representation is not normative.

The description defines:

~~~text
what CPU-visible classic hardware
must remain reachable
~~~

It does not define:

~~~text
which accesses must fault

which accesses must traverse the Bus

which accesses may use fast paths

which accesses may be shadowed
~~~

---

# 22. Coverage and Optimization Are Separate Concepts

The architecture must keep these concepts distinct:

~~~text
COVERAGE
    what CPU-visible hardware behavior exists

SEMANTICS
    what that hardware behavior means

ROUTING
    where the access is sent

OPTIMIZATION
    how the access is accelerated
~~~

Rigel owns:

~~~text
hardware semantics
classic behavior
interrupt authority
~~~

Bellatrix/Emu68 own:

~~~text
routing policy
MMU policy
fault handling
fast paths
caches
shadows
dispatch optimization
~~~

subject to semantic equivalence.

---

# 23. MMU Policy

Bellatrix may use existing Emu68 MMU mechanisms to intercept Rigel accesses.

For generic accesses:

~~~text
M68K access
     │
     ▼
MMU fault
     │
     ▼
Bellatrix Bus
     │
     ▼
Rigel
~~~

is valid.

But not every Rigel access must necessarily fault.

For example:

~~~text
INTENA / INTREQ
       │
       ▼
existing optimized Emu68 path
       │
       ▼
avoid MMU fault
       │
       ▼
preserve Rigel semantics
~~~

Therefore:

> **MMU interception is a fallback and routing mechanism, not a requirement that every Rigel transaction generate a fault.**

---

# 24. Bellatrix Bus

The Bellatrix Bus remains the generic host dispatch mechanism for accesses requiring generic host routing.

It must provide a complete fallback for Rigel-supported transactions assigned to the generic path.

However:

> **The Bellatrix Bus does not need to observe every transaction if a validated optimized path already preserves the required Rigel semantics.**

This explicitly permits the preservation of `INT_shadow`.

---

# 25. Generic Fallback Completeness

The generic fallback must be complete enough that optimization coverage does not define hardware visibility.

Conceptually:

~~~text
Rigel supports X
       │
       ▼
fast path handles X?
      / \
    yes  no
     │    │
     ▼    ▼
   fast   generic
   path   fallback
     │      │
     └──┬───┘
        ▼
correct semantics
~~~

Therefore:

~~~text
optimization miss
        !=
unmapped hardware
~~~

---

# 26. Fast Paths

Fast paths are explicitly permitted.

A fast path may bypass:

~~~text
MMU fault

fault reconstruction

generic provider lookup

generic Bus traversal

generic MMIO wrapper layers
~~~

provided that:

~~~text
observable(fast path)
        =
observable(reference semantics)
~~~

`INT_shadow` should be evaluated as one such specialized fast path.

---

# 27. Caches

Caches are explicitly permitted.

Examples include:

~~~text
provider cache

region cache

JIT MMIO classification cache

pre-resolved target

derived register-state cache
~~~

Caches must not eliminate hardware accesses whose reads or writes are semantically observable.

Caching routing metadata is distinct from caching hardware results.

---

# 28. Shadows

Shadows are explicitly permitted.

The intended relationship is:

~~~text
classic hardware authority
        │
        ▼
       Rigel
        │
        ▼
derived / synchronized
optimization state
        │
        ▼
       shadow
~~~

For Bellatrix:

~~~text
INT_shadow
    =
valuable Emu68 optimization
for INTENA / INTREQ access
when it avoids fault overhead
~~~

provided that:

~~~text
INT_shadow
    !=
independent interrupt controller
~~~

---

# 29. Direct and Specialized Paths

Specialized direct paths are permitted where semantically valid.

Conceptually:

~~~text
M68K access
     │
     ▼
specialized Emu68 handling
     │
     ▼
narrow Rigel integration
     │
     ▼
correct classic behavior
~~~

A specialized path may be preferable to forcing:

~~~text
specialized Emu68 handling
     │
     ▼
generic fault
     │
     ▼
generic Bus
     │
     ▼
generic MMIO
~~~

after the optimization has already identified the transaction.

---

# 30. Observable Equivalence

Every optimized path must satisfy observable equivalence.

For transaction `T`:

~~~text
reference =
    canonical Rigel semantics(T)

optimized =
    optimized path(T)
~~~

Required:

~~~text
observable(reference)
        =
observable(optimized)
~~~

Observable effects may include:

* return value;
* register state;
* interrupt state;
* IPL;
* DMA state;
* side effects;
* access ordering;
* timing-visible effects;
* subsequent chipset behavior.

---

# 31. MMIO Width and Ordering

All paths must preserve:

* M68K transaction width;
* alignment semantics;
* repeated accesses;
* read/write distinction;
* side effects;
* transaction ordering;
* valid decomposition rules.

An optimized path does not gain permission to change M68K hardware semantics.

---

# 32. Address Namespace Separation

The API must distinguish:

~~~text
M68K CPU-visible MMIO address

chipset-generated DMA address

guest physical address

host pointer
~~~

`INT_shadow` is host optimization state.

It does not create a new guest-visible address namespace.

---

# 33. Memory Boundary

Rigel DMA continues to use host-provided guest physical memory operations.

~~~text
Rigel DMA
    │
    ▼
guest physical address
    │
    ▼
host memory operations
    │
    ▼
Bellatrix guest memory
~~~

This is separate from CPU MMIO routing and shadow optimization.

---

# 34. Host Memory and JIT Coherency

Bellatrix owns host-specific memory coherency.

~~~text
Rigel DMA write
      │
      ▼
host mem_write
      │
      ▼
Bellatrix
      │
      ├── update guest RAM
      └── invalidate Emu68 translation
          if required
~~~

Rigel must not acquire JIT-specific knowledge.

---

# 35. Timing

Rigel remains authoritative for classic chipset time.

~~~text
host CPU progress
       │
       ▼
Rigel advance
       │
       ▼
classic chipset timeline
~~~

Caches, shadows, and fast paths must not create an independent chipset timeline.

---

# 36. Interrupt Ownership

Rigel owns:

~~~text
classic interrupt sources

INTREQ semantics

INTENA semantics

classic priority resolution

Rigel IPL
~~~

Emu68 may own:

~~~text
INT_shadow implementation

optimized access recognition

fast-path execution machinery

CPU interrupt input plumbing
~~~

The distinction is:

~~~text
hardware meaning
    = Rigel

optimized delivery
    = Emu68 / Bellatrix
~~~

---

# 37. Native and Rigel Interrupt Domains

Bellatrix keeps native and compatibility interrupt domains distinct.

~~~text
native interrupt domain
         │
         ▼
      native IPL
          │
          ├──────┐
                 ▼
           IPL arbitration
                 ▲
                 │
             Rigel IPL
                 ▲
                 │
        classic Rigel domain
~~~

Then:

~~~text
effective IPL
      │
      ▼
Emu68 CPU interrupt input
~~~

`INT_shadow` must not collapse the native and classic domains.

---

# 38. `INT_shadow` Integration Requirement

The existing Emu68 `INT_shadow` path should be **preserved provisionally as a performance optimization**, then adapted as required for Rigel ownership.

The integration review must determine:

1. Which `INTENA` and `INTREQ` transactions use `INT_shadow`.
2. Which transactions avoid an MMU fault because of this path.
3. Which reads are served from shadow state.
4. Which writes update shadow state.
5. What minimum action is required to reflect those writes in Rigel.
6. Whether a narrow Rigel fast-path API would preserve the optimization better than generic MMIO.
7. How Rigel-originated `INTREQ` changes affect the shadow.
8. How reset establishes coherence.
9. Whether the shadow participates in CPU-visible reads.
10. Whether the shadow influences IPL calculation today.
11. How to ensure Rigel IPL remains authoritative.
12. Whether a miss falls back to the generic MMU/Bus path.
13. Whether any synchronization design accidentally reintroduces the fault being avoided.
14. Whether the resulting optimized path materially improves performance relative to generic dispatch.

The preferred direction is:

~~~text
                 CPU INT access
                       │
                       ▼
               Emu68 INT_shadow
                   fast path
                       │
                       ▼
             narrow/coherent Rigel
               semantic update
                       │
                       ▼
                     Rigel
                       │
                INTENA / INTREQ
                       │
                       ▼
                  Rigel IPL
                       │
                       ▼
                 Emu68 INT.IPL
~~~

with generic fallback:

~~~text
shadow path unavailable
         │
         ▼
      MMU fault
         │
         ▼
   Bellatrix Bus
         │
         ▼
 canonical Rigel MMIO
~~~

---

# 39. Avoid Defeating `INT_shadow`

The Bellatrix integration must not preserve `INT_shadow` only nominally while forcing every shadowed access to perform the complete generic fault-equivalent path anyway.

That would provide:

~~~text
complexity of shadow synchronization
        +
cost of generic dispatch
~~~

without preserving the performance benefit.

The desired design is:

~~~text
INT_shadow hit
      │
      ▼
cheap optimized path
      │
      ▼
correct Rigel semantics
~~~

while:

~~~text
INT_shadow miss
      │
      ▼
generic fallback
~~~

The exact mechanism should be selected after inspecting the existing Emu68 implementation and profiling the integration.

---

# 40. Optimization Correctness Tests

For every optimized transaction class:

~~~text
reference path
      │
      ▼
capture observable behavior

optimized path
      │
      ▼
capture observable behavior

compare
~~~

Required:

~~~text
equivalent
~~~

This is particularly important for:

~~~text
INTENA writes

INTREQ writes

INTREQ reads where applicable

interrupt acknowledgement

Rigel-generated requests

IPL transitions

reset

repeated accesses
~~~

---

# 41. Fault-Avoidance Test

For optimized paths intended to avoid fault overhead, correctness alone is not sufficient.

The test should also establish that the intended optimization remains effective.

For example:

~~~text
M68K access INTENA
        │
        ▼
INT_shadow fast path
        │
        ▼
no generic MMU fault
        │
        ▼
correct Rigel semantics
~~~

The implementation should be able to demonstrate separately:

~~~text
semantic correctness

and

fault avoidance
~~~

where fault avoidance is the purpose of the optimization.

---

# 42. Coverage Correctness Test

Let:

~~~text
R = Rigel-supported CPU-visible transactions
~~~

For every representative transaction in `R`:

~~~text
optimized path available?
       │
      / \
    yes  no
     │    │
     ▼    ▼
optimized fallback
     │      │
     └──┬───┘
        ▼
correct result
~~~

A test must fail if a supported access reaches:

~~~text
unmapped

incorrect provider

silent drop

host fault with no valid fallback
~~~

merely because an optimization does not cover it.

---

# 43. Shadow Correctness Test

For `INT_shadow`, tests should explicitly exercise:

~~~text
CPU writes INTENA

CPU writes INTREQ

repeated INTENA/INTREQ accesses

Rigel device raises interrupt

Rigel device clears interrupt

interrupt acknowledgement

reset

rapid interrupt transitions

shadow hit

shadow miss

generic fallback

Rigel IPL changes
~~~

At each observation point:

~~~text
CPU-visible behavior
        =
Rigel-defined behavior
~~~

Where the optimized path is intended to avoid faults, tests should additionally confirm:

~~~text
shadow hit
        =>
generic MMU fault avoided
~~~

---

# 44. Performance Is a Design Requirement

Correctness does not imply forcing every hardware access through the generic path.

The architecture should explicitly preserve the ability to optimize:

~~~text
correctness
    │
    ▼
semantic reference
    │
    ▼
validated optimization
    │
    ▼
performance
~~~

The wrong model is:

~~~text
correctness
    =
everything must fault
    =
everything must traverse Bellatrix Bus
~~~

The correct model is:

~~~text
correctness
    =
all supported hardware remains semantically reachable
~~~

while:

~~~text
implementation
    =
free to optimize
~~~

For `INTENA` and `INTREQ`, this means that preserving the existing fault-avoidance behavior of `INT_shadow` is a legitimate design goal.

---

# 45. Video

Rigel continues to own classic video generation.

~~~text
Chip RAM
   │
   ▼
Agnus / Denise
   │
   ▼
classic pixels
   │
   ▼
host-consumable output
~~~

Rigel remains independent from:

~~~text
RTG
P96
VC4
AROS native graphics
physical framebuffer
~~~

---

# 46. Audio and Input

Audio:

~~~text
Paula
  │
  ▼
Rigel
  │
  ▼
host-independent audio output
~~~

Input:

~~~text
native input
    │
    ▼
Bellatrix
    │
    ▼
classic input representation
    │
    ▼
Rigel
~~~

---

# 47. Concurrency

Rigel defines serialization requirements, not host placement.

The baseline rule is:

> A Rigel instance is non-concurrent unless otherwise documented. The host is responsible for serialization.

Bellatrix may execute Rigel:

~~~text
same core

different ARM core

worker thread

queue consumer
~~~

without changing the Rigel API.

Fast paths and shadow integration must respect the same serialization contract.

---

# 48. Bellatrix Adapter Responsibilities

The Bellatrix adapter should:

~~~text
create/configure Rigel

obtain Rigel CPU-visible coverage

guarantee complete semantic reachability

configure MMU interception where required

register generic Bus fallback coverage

forward canonical MMIO where required

preserve validated Emu68 fast paths

preserve INT_shadow fault avoidance
where semantically valid

provide narrow optimized integration
where justified

preserve generic fallback behavior

provide guest memory

report execution progress

consume deadlines

obtain Rigel IPL

publish Rigel IPL to Emu68 CPU input

consume video/audio

adapt input
~~~

It must not implement:

~~~text
independent Amiga register semantics

independent INTENA authority

independent INTREQ authority

Copper semantics

Blitter semantics

CIA semantics

Paula semantics

Agnus DMA semantics
~~~

---

# 49. Emu68 Optimization Responsibilities

Emu68 may provide:

~~~text
JIT-side fast paths

MMIO classification caches

INT_shadow

other shadows

specialized register handling

fault bypass

provider caches
~~~

provided that:

~~~text
optimization hit
      │
      ▼
correct Rigel semantics
      │
      ▼
avoid unnecessary generic overhead


optimization miss
      │
      ▼
correct fallback
      │
      ▼
correct Rigel semantics
~~~

An optimization must never turn a supported Rigel transaction into an inaccessible transaction.

---

# 50. Migration Strategy

Recommended sequence:

~~~text
1. Capture behavioral baseline

2. Inventory Rigel CPU-visible hardware

3. Establish authoritative Rigel coverage

4. Establish canonical Rigel MMIO semantics

5. Inventory existing Emu68 optimized paths

6. Inspect INT_shadow implementation in detail

7. Determine exactly which faults INT_shadow avoids

8. Identify its current INTENA/INTREQ semantics

9. Separate:
      hardware authority
      shadow state
      CPU interrupt input

10. Preserve Rigel ownership of:
      INTENA
      INTREQ
      interrupt sources
      priority resolution
      Rigel IPL

11. Preserve INT_shadow as an optimization
    where correctness allows

12. Define the cheapest coherent Rigel interaction
    for INT_shadow hits

13. Ensure shadow hits do not unnecessarily
    fall back through the full fault path

14. Establish generic MMU/Bus fallback

15. Validate shadow misses

16. Validate Rigel-originated INTREQ changes

17. Validate reset coherence

18. Validate Rigel IPL → Emu68 INT.IPL

19. Benchmark fault avoidance

20. Migrate harness

21. Migrate Bellatrix

22. Compare optimized and reference behavior

23. Freeze API Version 1
~~~

---

# 51. Conformance Invariants

## Visibility

~~~text
Rigel supports transaction T
        =>
Bellatrix can execute T correctly
~~~

## Path independence

~~~text
correctness(T)
does not require
one specific physical route
~~~

## Optimization fallback

~~~text
optimization miss
        !=
hardware absent
~~~

## Fault avoidance

~~~text
optimized path intended to avoid fault
        =>
should not unnecessarily re-enter
the generic fault path
~~~

## Shadow authority

~~~text
INT_shadow
    =
optimization

INT_shadow
    !=
independent interrupt authority
~~~

## Interrupt authority

~~~text
Rigel owns:
    INTENA semantics
    INTREQ semantics
    interrupt sources
    priority resolution
    classic IPL
~~~

## CPU interrupt delivery

~~~text
Rigel IPL
    │
    ▼
Emu68 INT.IPL
~~~

## Emu68 optimization freedom

~~~text
Emu68 may optimize
provided observable Rigel semantics
are preserved
~~~

---

# 52. Review Checklist

Every relevant patch should answer:

1. Does Rigel support this CPU-visible access?
2. Can M68K software still perform it correctly?
3. Which path handles it?
4. Is that path generic or optimized?
5. Does the optimized path avoid a fault?
6. Is avoiding that fault intentional and valuable?
7. What happens on an optimization miss?
8. Is there a correct generic fallback?
9. Is an MMU fault actually required?
10. Is generic Bus traversal actually required?
11. Could a direct specialized Rigel path preserve semantics more efficiently?
12. Could a cache alter observable MMIO behavior?
13. Does a shadow represent optimization state or hardware authority?
14. Can shadow and Rigel state diverge?
15. Does `INT_shadow` cover `INTENA`?
16. Does `INT_shadow` cover `INTREQ`?
17. Which operations does `INT_shadow` accelerate?
18. Which faults does `INT_shadow` avoid?
19. How is `INT_shadow` synchronized with Rigel?
20. Can Rigel-originated `INTREQ` changes invalidate shadow assumptions?
21. Does synchronization accidentally reintroduce the generic fault?
22. Is Rigel IPL still authoritative?
23. Is Bellatrix reconstructing IPL from `INT_shadow`?
24. Is Rigel IPL delivered through Emu68 `INT.IPL`?
25. Does an optimization accidentally define hardware coverage?
26. Does an absent optimization entry make hardware inaccessible?
27. Are MMIO width and ordering preserved?
28. Are side effects preserved?
29. Are timing-visible consequences preserved?
30. Are guest physical addresses separate from CPU MMIO addresses?
31. Does Rigel remain host-independent?
32. Can the harness exercise the reference path?
33. Can optimized and reference paths be compared?
34. Can performance tests confirm fault avoidance?
35. Does the optimization improve performance without changing hardware behavior?

---

# 53. Target Architecture

~~~text
                        M68K CPU
                           │
                           ▼
                         Emu68
                           │
          ┌────────────────┼─────────────────┐
          │                │                 │
          ▼                ▼                 ▼
       caches          fast paths        INT_shadow
                                             │
                                  optimized INTENA/INTREQ
                                      fault avoidance
          │                │                 │
          └────────────────┼─────────────────┘
                           │
                    handled correctly?
                           │
                         /   \
                       yes    no
                        │      │
                        │      ▼
                        │   generic path
                        │      │
                        │      ▼
                        │  MMU / fault
                        │      │
                        │      ▼
                        │ Bellatrix Bus
                        │      │
                        │      ▼
                        │ canonical MMIO
                        │      │
                        └──┬───┘
                           │
                           ▼
                         Rigel
                           │
              authoritative classic semantics
                           │
          ┌────────────────┼─────────────────┐
          │                │                 │
          ▼                ▼                 ▼
        custom            CIA             other
        hardware        hardware        hardware
          │                │                 │
          └────────────────┼─────────────────┘
                           │
                           ▼
                    interrupt sources
                           │
                   ┌───────┴───────┐
                   ▼               ▼
                INTENA           INTREQ
                   │               │
                   └───────┬───────┘
                           ▼
                  priority resolution
                           │
                           ▼
                       Rigel IPL
                           │
                           ▼
                    Emu68 INT.IPL
                           │
                           ▼
                        M68K CPU
~~~

The optimized interrupt path is therefore:

~~~text
M68K INTENA / INTREQ access
           │
           ▼
      Emu68 INT_shadow
           │
           ▼
       fast handling
     without generic fault
           │
           ▼
      coherent Rigel
     semantic transition
           │
           ▼
        Rigel IPL
           │
           ▼
      Emu68 INT.IPL
~~~

while the fallback remains:

~~~text
unoptimized Rigel access
          │
          ▼
       MMU fault
          │
          ▼
    Bellatrix Bus
          │
          ▼
 canonical Rigel MMIO
          │
          ▼
        Rigel
~~~

The exact optimized integration mechanism is implementation-defined.

The ownership relationship is not.

---

# 54. Definition of Done for Rigel API Version 1

API Version 1 should not be frozen until:

* Rigel has an authoritative CPU-visible hardware definition;
* every Rigel-supported CPU-visible access remains reachable;
* canonical MMIO defines reference semantics;
* canonical MMIO is not unnecessarily mandated as the physical path for every access;
* Bellatrix has a complete generic fallback;
* MMU interception is used where required rather than universally mandated;
* Bellatrix Bus routing is used where required rather than universally mandated;
* caches remain possible;
* fast paths remain possible;
* shadows remain possible;
* specialized Emu68 paths remain possible;
* `INT_shadow` is preserved when it provides useful fault avoidance and can remain correct;
* `INT_shadow` hits do not unnecessarily enter the generic fault path;
* optimization misses correctly fall back;
* optimized paths preserve observable Rigel semantics;
* `INT_shadow` cannot become an independent interrupt authority;
* `INTENA` semantics remain Rigel-owned;
* `INTREQ` semantics remain Rigel-owned;
* Rigel-generated interrupt changes cannot silently diverge from shadow-visible behavior;
* synchronization semantics are explicit;
* synchronization does not unnecessarily destroy the performance advantage of the shadow;
* Rigel IPL remains authoritative for the classic interrupt domain;
* Rigel IPL is delivered to the CPU through the appropriate Emu68 `INT.IPL` path;
* Bellatrix does not reconstruct classic IPL from Emu68 shadow state;
* DMA ownership remains unchanged;
* timing ownership remains unchanged;
* Rigel remains host-topology neutral;
* harness and Bellatrix use compatible production semantics;
* optimized and reference paths have behavioral equivalence tests;
* performance tests can demonstrate that intended fast paths actually avoid generic fault overhead.

For Version 1:

> Stable public source-level API. Binary ABI stability is not implied unless separately documented.

---

# 55. Final Recommendation

The Rigel/Bellatrix/Emu68 relationship should be governed by four distinct concepts:

~~~text
1. HARDWARE AUTHORITY

       Rigel
         │
         ▼
   classic hardware
      semantics


2. REACHABILITY

      Bellatrix
         │
         ▼
   guarantees every
   Rigel-supported
   CPU-visible access
   remains usable


3. OPTIMIZATION

       Emu68
         │
         ├── caches
         ├── fast paths
         ├── shadows
         ├── INT_shadow
         └── specialized paths


4. CPU INTERRUPT DELIVERY

      Rigel IPL
         │
         ▼
     Emu68 INT.IPL
         │
         ▼
        M68K
~~~

These concepts must not be collapsed into one another.

In particular:

> **Rigel defines what the classic hardware means.**

> **Bellatrix guarantees that supported hardware remains reachable.**

> **Emu68 remains free to optimize how CPU accesses are executed.**

> **Rigel publishes the resolved classic interrupt result through IPL rather than requiring Emu68 to reconstruct it from shadow state.**

For `INTENA` and `INTREQ`, the desired model is:

~~~text
                    CPU
                     │
              INTENA / INTREQ
                     │
                     ▼
                   Emu68
                     │
                     ▼
                INT_shadow
                     │
             optimized fast path
             avoiding generic fault
                     │
                     ▼
              coherent Rigel
             interrupt semantics
                     │
             ┌───────┴───────┐
             ▼               ▼
          INTENA           INTREQ
             │               │
             └───────┬───────┘
                     ▼
            priority resolution
                     │
                     ▼
                 Rigel IPL
                     │
                     ▼
               Emu68 INT.IPL
                     │
                     ▼
                    M68K
~~~

with the generic path remaining available for accesses not handled by an optimized mechanism:

~~~text
M68K access
     │
     ▼
optimization unavailable
     │
     ▼
MMU / fault
     │
     ▼
Bellatrix Bus
     │
     ▼
canonical Rigel MMIO
     │
     ▼
Rigel semantics
~~~

The final compatibility rule is:

> **No software should fail because it accessed a Rigel-supported classic hardware address that was missing from a Bellatrix or Emu68 optimization path.**

The final performance rule is:

> **Satisfying complete Rigel hardware visibility must not require disabling valid Emu68 caches, fast paths, shadows, specialized handlers, or other mechanisms that avoid unnecessary fault overhead.**

And the specific interrupt rule is:

> **Preserve `INT_shadow` as a fault-avoidance optimization where it remains beneficial, preserve Rigel as the authority for `INTENA`/`INTREQ` semantics, and use Rigel's resolved IPL as the architectural interrupt input to Emu68.**

Thus the shortest statement of the target architecture is:

> **Semantic reachability is mandatory. Generic dispatch is the fallback. Fast paths are allowed. Rigel owns the hardware.**
