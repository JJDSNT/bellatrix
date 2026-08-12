# Rigel API Convergence Plan

## Refining the Existing Rigel Host Boundary for Bellatrix Integration

**Status:** Proposed API Boundary Refinement Baseline  
**Target:** Rigel API Version 1  
**Related specification:** `Bellatrix/docs/Rigel_integration.md`

---

# 1. Purpose

This document defines the recommended refinement of the existing Rigel public API so that the boundary between Rigel and its hosts is explicit, minimal, host-neutral, and suitable for long-term use by Bellatrix and the standalone harness.

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

> Preserve the existing Rigel execution and hardware boundary while refining the public host interface so that every CPU-visible classic hardware access implemented by Rigel remains correctly reachable from M68K software.

The fundamental compatibility requirement is:

> **No M68K software may fail merely because it accesses a CPU-visible classic hardware address implemented by Rigel that Bellatrix failed to expose or route correctly.**

This requirement concerns **semantic reachability**, not a mandatory physical dispatch path.

Bellatrix and Emu68 remain free to implement:

* caches;
* shadows;
* fast paths;
* direct paths;
* specialized handlers;
* MMU interception;
* fault-based dispatch;
* generic bus dispatch;

provided that these mechanisms preserve the observable Rigel hardware semantics.

Therefore:

> **Complete hardware visibility is mandatory. A particular implementation path is not.**

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

That path is valid, but it is not mandatory.

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

---

# 5. Existing Emu68 Optimization Mechanisms

Emu68 already contains specialized handling and optimization mechanisms associated with classic Amiga hardware operation.

These mechanisms are not inherently incompatible with Rigel.

They should be treated as:

~~~text
Emu68 implementation mechanisms
~~~

rather than:

~~~text
authoritative definitions
of classic hardware semantics
~~~

Examples include conceptually:

~~~text
fast paths

specialized MMIO handling

cached state

shadow state

INT_shadow

JIT optimizations

fault bypasses
~~~

Bellatrix integration should preserve useful Emu68 optimizations wherever they remain semantically correct.

The objective is **not** to force all classic hardware traffic through a slow generic dispatcher.

---

# 6. Emu68 `INT_shadow`

Emu68 contains shadow handling associated with classic interrupt registers, including the existing handling around:

~~~text
INTENA

INTREQ
~~~

This mechanism is relevant to Bellatrix/Rigel integration because Rigel itself owns the authoritative classic interrupt state.

The required authority relationship is:

~~~text
                  Rigel
                    │
          authoritative interrupt
               semantics/state
                    │
          ┌─────────┴─────────┐
          │                   │
          ▼                   ▼
       INTENA               INTREQ
          │                   │
          └─────────┬─────────┘
                    │
                    ▼
               Rigel IPL
~~~

Any Emu68 shadow associated with these registers must therefore be treated as:

~~~text
optimization / mirror / acceleration state
~~~

and not as:

~~~text
a second authoritative interrupt controller
~~~

---

# 7. Shadow Authority Rule

The fundamental rule for shadows is:

> **A shadow may accelerate access to Rigel-owned state, but it must not become an independent source of classic hardware truth.**

Conceptually:

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
   normal Rigel path        synchronized
                             Emu68 shadow
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

# 8. `INT_shadow` Must Not Define Coverage

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
     ├── shadow path if available
     │
     └── fallback path otherwise
~~~

Therefore:

> **Shadow coverage may be incomplete. Hardware visibility may not be incomplete.**

---

# 9. Shadow Miss Must Fall Back Correctly

If an optimized Emu68 path does not recognize a Rigel-supported access, that must not make the access disappear.

Conceptually:

~~~text
M68K access
     │
     ▼
optimized path?
     │
    / \
  yes  no
   │    │
   ▼    ▼
fast   fallback
path    path
   │      │
   └──┬───┘
      │
      ▼
correct Rigel semantics
~~~

A shadow miss is therefore not equivalent to:

~~~text
unmapped hardware
~~~

It means only:

~~~text
this optimization did not handle
the transaction
~~~

The generic integration path must remain capable of handling the transaction when Rigel supports it.

---

# 10. `INTENA` / `INTREQ` Special Case

`INTENA` and `INTREQ` require particular care because they directly influence classic interrupt state.

The ownership model is:

~~~text
M68K write/read
      │
      ▼
Bellatrix / Emu68
      │
      ├── optimized/shadow path
      │
      └── generic path
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
~~~

Regardless of the path used, the resulting state must be equivalent to the state produced by the authoritative Rigel implementation.

---

# 11. Shadow Synchronization

If `INT_shadow` remains active in Bellatrix builds, its synchronization semantics must be explicit.

At minimum, the implementation must establish:

~~~text
who updates the shadow

when the shadow is updated

whether reads may be satisfied from it

whether writes may be absorbed by it

how Rigel-originated changes are reflected

how reset initializes it

how asynchronous interrupt-source changes
affect its validity
~~~

The critical invariant is:

~~~text
shadow-visible state
        │
        ▼
must never contradict
authoritative Rigel behavior
~~~

---

# 12. Prefer Derived or Invalidatable Shadows

Where practical, shadow state should behave as:

~~~text
derived state
~~~

rather than:

~~~text
parallel hardware state
~~~

Conceptually:

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
       fast read
~~~

or:

~~~text
state change
     │
     ▼
invalidate shadow
     │
     ▼
next access refreshes
from authoritative state
~~~

The exact implementation is not prescribed.

What matters is maintaining one semantic authority.

---

# 13. Optimized Writes

A fast or shadowed write path is permitted.

For example:

~~~text
M68K write INTENA
       │
       ▼
Emu68 optimized path
       │
       ├── update acceleration state
       │
       └── preserve Rigel write semantics
                    │
                    ▼
               authoritative
                  Rigel
~~~

The optimization must not merely update an Emu68-local shadow while leaving Rigel unaware of a hardware-visible state change.

Unless the optimization is formally implemented as part of a coherent Rigel transaction mechanism, the authoritative Rigel state must observe the operation.

---

# 14. Rigel-Originated Interrupt Changes

Not all interrupt-state changes originate from CPU writes.

Classic devices may generate interrupt requests.

Conceptually:

~~~text
Paula / CIA / Blitter / etc.
             │
             ▼
           Rigel
             │
             ▼
           INTREQ
             │
             ▼
        resolved IPL
~~~

Therefore an Emu68 shadow cannot assume that interrupt state changes only when the CPU accesses `INTREQ` or `INTENA`.

Any cached or shadowed representation must account for Rigel-originated state transitions.

This is one reason why the shadow cannot be authoritative.

---

# 15. Rigel IPL Remains Authoritative

Bellatrix must consume Rigel's resolved compatibility IPL.

Conceptually:

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
Emu68 / M68K
~~~

Bellatrix must not reconstruct Rigel IPL from `INT_shadow`.

`INT_shadow` may participate in optimization, but:

> **Rigel IPL is the architectural interrupt result.**

---

# 16. Canonical MMIO as Semantic Reference

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

This interface defines the normal semantic boundary.

However:

> **Canonical MMIO is a semantic reference path, not necessarily the physical path taken by every optimized access.**

An optimized path is permitted when it remains observationally equivalent.

---

# 17. CPU-Visible Hardware Coverage

Rigel should provide an authoritative description of its CPU-visible classic hardware coverage.

Conceptually:

~~~c
struct rigel_mmio_region {
    uint32_t start;
    uint32_t end;
    uint32_t flags;
};
~~~

The exact representation is not normative.

This description defines:

~~~text
what CPU-visible classic hardware Rigel supports
~~~

It does not prescribe:

~~~text
how every access must physically travel
through Bellatrix/Emu68
~~~

---

# 18. Coverage and Optimization Are Separate Concepts

These concepts must remain distinct:

~~~text
COVERAGE
    what hardware accesses exist

SEMANTICS
    what those accesses mean

ROUTING
    where accesses are sent

OPTIMIZATION
    how efficiently they are handled
~~~

Rigel primarily owns:

~~~text
COVERAGE
SEMANTICS
~~~

Bellatrix/Emu68 primarily own:

~~~text
ROUTING
OPTIMIZATION
~~~

subject to preservation of Rigel semantics.

---

# 19. MMU Policy

Bellatrix may use existing Emu68 MMU mechanisms to intercept Rigel accesses.

But not every Rigel access must necessarily fault through the MMU.

Conceptually:

~~~text
Rigel-supported access
        │
        ▼
optimized direct handling available?
       / \
     yes  no
      │    │
      ▼    ▼
   optimized   MMU/fault
      │           │
      │           ▼
      │      Bellatrix Bus
      │           │
      └─────┬─────┘
            ▼
      Rigel semantics
~~~

The MMU is therefore a mechanism for reachability, not the definition of hardware visibility.

---

# 20. Bellatrix Bus

The Bellatrix Bus remains the generic host dispatch mechanism for accesses requiring host dispatch.

It must provide a complete fallback path for Rigel-supported accesses that are assigned to it.

However:

> **The Bellatrix Bus does not need to observe every transaction if another validated path already preserves the same Rigel semantics.**

This permits:

* shadows;
* fast paths;
* cached provider lookup;
* specialized handlers;
* future optimizations.

---

# 21. Fast Paths

Fast paths are explicitly permitted.

A fast path may bypass:

~~~text
generic provider lookup

generic Bus traversal

fault reconstruction

some generic MMIO wrapper layers
~~~

provided that:

~~~text
fast-path semantics
        =
canonical Rigel semantics
~~~

Fast paths must have a correct fallback mechanism for accesses they do not handle.

---

# 22. Caches

Caches are explicitly permitted.

Examples include:

~~~text
provider lookup cache

region cache

dispatch cache

derived-state cache

JIT-side MMIO classification cache
~~~

Caches may accelerate:

~~~text
where should this access go?
~~~

or, when semantically safe:

~~~text
what result can be reused?
~~~

They must not incorrectly cache hardware operations whose reads or writes have observable side effects.

---

# 23. Shadows

Shadows are explicitly permitted when they preserve Rigel semantics.

A shadow is conceptually:

~~~text
authoritative Rigel state
        │
        ▼
derived / synchronized representation
        │
        ▼
optimized host access
~~~

A shadow must not become:

~~~text
a competing hardware implementation
~~~

The existing Emu68 `INT_shadow` should be reviewed under precisely this rule.

---

# 24. Observable Equivalence

Every optimized path must satisfy observable equivalence.

For a transaction `T`:

~~~text
reference_result =
    Rigel_semantics(T)

optimized_result =
    optimized_path(T)
~~~

Required:

~~~text
observable(reference_result)
        =
observable(optimized_result)
~~~

Observable state includes, where applicable:

* returned value;
* register state;
* interrupt state;
* IPL;
* DMA state;
* side effects;
* ordering;
* timing-visible consequences;
* subsequent hardware behavior.

---

# 25. MMIO Width and Ordering

Optimization must preserve:

* M68K transaction width;
* alignment semantics;
* access ordering;
* side effects;
* repeated accesses;
* read/write distinctions;
* decomposition semantics.

The execution engine must not freely:

~~~text
eliminate
duplicate
combine
split
reorder
cache
~~~

hardware transactions unless the Rigel contract explicitly permits it.

---

# 26. Address Namespace Separation

The API must distinguish:

~~~text
M68K CPU-visible MMIO address

chipset-generated DMA address

guest physical address

host pointer
~~~

`INT_shadow` and other optimization state do not alter these namespaces.

---

# 27. Memory Boundary

Rigel DMA uses host-provided guest physical memory operations.

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

This is independent from CPU MMIO routing.

---

# 28. Host Memory and JIT Coherency

Bellatrix owns host-specific memory coherency.

For example:

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

Rigel must not acquire knowledge of Emu68 JIT internals.

---

# 29. Timing

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

Optimization mechanisms must not introduce an independent classic hardware timeline.

---

# 30. Interrupt Ownership

Rigel owns:

~~~text
classic interrupt sources

INTREQ

INTENA

priority resolution

Rigel IPL
~~~

Emu68 may contain optimized representations associated with interrupt handling.

Those representations remain subordinate to Rigel's authoritative semantics in Bellatrix/Rigel integration.

---

# 31. Native and Rigel Interrupt Domains

Bellatrix keeps native and classic interrupt domains distinct.

~~~text
native host interrupt domain
           │
           ├──────────┐
           │          │
           ▼          ▼
       native IPL   arbitration
                      ▲
                      │
                  Rigel IPL
                      ▲
                      │
                Rigel interrupt
                    domain
~~~

The existing Emu68 interrupt optimization mechanisms must not collapse these ownership domains.

---

# 32. Video

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

# 33. Audio and Input

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

# 34. Concurrency

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

---

# 35. Bellatrix Adapter Responsibilities

The Bellatrix adapter should:

~~~text
create/configure Rigel

obtain Rigel CPU-visible coverage

guarantee complete semantic reachability

configure required MMU interception

register generic Bus fallback coverage

forward canonical MMIO where required

permit validated optimized paths

preserve fallback behavior

provide guest memory

report execution progress

consume deadlines

obtain Rigel IPL

consume video/audio

adapt input
~~~

It must not implement:

~~~text
Amiga register semantics

independent INTENA semantics

independent INTREQ semantics

Copper semantics

Blitter semantics

CIA semantics

Paula semantics

Agnus DMA semantics
~~~

---

# 36. Emu68 Optimization Responsibilities

Emu68 may provide:

~~~text
JIT-side fast paths

MMIO classification caches

shadows

specialized register handling

fault bypass

provider caches
~~~

provided that Bellatrix integration guarantees:

~~~text
optimization hit
      │
      ▼
correct Rigel semantics

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

# 37. `INT_shadow` Integration Requirement

The existing Emu68 `INT_shadow` handling must be explicitly reviewed during Bellatrix integration.

The review must determine:

1. Which `INTENA` and `INTREQ` accesses use the shadow.
2. Whether reads are satisfied from shadow state.
3. Whether writes modify shadow state.
4. Whether writes also reach authoritative Rigel semantics.
5. How Rigel-generated interrupt requests update or invalidate the shadow.
6. How reset synchronizes the shadow.
7. Whether shadow state influences IPL calculation.
8. Whether any path can cause Rigel and `INT_shadow` to diverge.
9. Whether a shadow miss correctly reaches the generic Rigel path.
10. Whether removing the shadow would alter correctness or only performance.

The desired result is:

~~~text
                 Rigel
                   │
             authoritative
            INTENA / INTREQ
                   │
          ┌────────┴────────┐
          │                 │
          ▼                 ▼
    canonical path      synchronized
                         INT_shadow
                             │
                             ▼
                          fast path
~~~

not:

~~~text
Rigel interrupts       Emu68 interrupts
      │                       │
      ▼                       ▼
 authority A              authority B
~~~

---

# 38. Optimization Correctness Test

For every optimized transaction class:

~~~text
run through reference path
           │
           ▼
capture observable result

run through optimized path
           │
           ▼
capture observable result

compare
~~~

Required:

~~~text
equivalent
~~~

This is particularly important for:

~~~text
INTENA

INTREQ

interrupt acknowledgement

IPL transitions

repeated register reads

side-effecting registers
~~~

---

# 39. Coverage Correctness Test

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

A test must fail if a Rigel-supported access reaches:

~~~text
unmapped

unhandled

silent drop
~~~

merely because an optimization table does not contain it.

---

# 40. Shadow Correctness Test

For `INT_shadow`, tests should explicitly exercise:

~~~text
CPU writes INTENA

CPU writes INTREQ

CPU reads relevant state

Rigel device raises interrupt

Rigel device clears interrupt

interrupt acknowledgement

reset

rapid interrupt transitions

shadow hit

shadow miss

fallback path
~~~

At each observation point:

~~~text
CPU-visible behavior
        =
Rigel-defined behavior
~~~

---

# 41. Performance Is a Design Requirement

Correctness does not imply forcing every hardware access through the most generic path.

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
everything must traverse generic Bus
~~~

The correct model is:

~~~text
correctness
    =
all supported hardware remains semantically reachable
```

while:

~~~text
implementation
    =
free to optimize
~~~

---

# 42. Migration Strategy

Recommended sequence:

~~~text
1. Capture behavioral baseline

2. Inventory Rigel CPU-visible hardware

3. Establish authoritative Rigel coverage

4. Establish canonical Rigel MMIO semantics

5. Inventory existing Emu68 specialized paths

6. Identify caches, shadows and fast paths

7. Explicitly inspect INT_shadow behavior

8. Classify each optimized path:

      preserve
      adapt
      synchronize
      invalidate
      replace
      remove

9. Establish generic fallback coverage

10. Configure MMU interception where required

11. Configure Bellatrix Bus fallback

12. Preserve validated fast paths

13. Validate optimization misses

14. Validate INTENA/INTREQ synchronization

15. Validate Rigel IPL ownership

16. Migrate harness

17. Migrate Bellatrix

18. Compare optimized and reference behavior

19. Freeze API Version 1
~~~

---

# 43. Conformance Invariants

## Visibility

~~~text
Rigel supports transaction T
        =>
Bellatrix can execute T correctly
~~~

## Path independence

~~~text
correctness(T)
does not depend on
T using one specific dispatch path
~~~

## Optimization fallback

~~~text
optimization miss
        !=
hardware absent
~~~

## Shadow authority

~~~text
shadow
    !=
independent hardware authority
~~~

## Interrupt authority

~~~text
Rigel
    owns INTENA / INTREQ semantics
    and resolved classic IPL
~~~

## Emu68 optimization freedom

~~~text
Emu68 may optimize
provided observable Rigel semantics
are preserved
~~~

---

# 44. Review Checklist

Every relevant patch should answer:

1. Does Rigel support this CPU-visible access?
2. Can M68K software still perform it?
3. Which path handles it?
4. Is that path generic or optimized?
5. What happens on an optimization miss?
6. Is there a correct fallback?
7. Is an MMU fault actually required?
8. Is generic Bus traversal actually required?
9. Could a fast path preserve semantics more efficiently?
10. Could a cache alter observable MMIO behavior?
11. Does a shadow represent derived state or independent state?
12. Can shadow and Rigel state diverge?
13. Does `INT_shadow` affect `INTENA`?
14. Does `INT_shadow` affect `INTREQ`?
15. How is `INT_shadow` synchronized?
16. Can Rigel-originated interrupt changes invalidate shadow state?
17. Is Rigel IPL still authoritative?
18. Is Bellatrix reconstructing IPL from shadow state?
19. Does an optimization accidentally define hardware coverage?
20. Does an absent optimization entry make hardware inaccessible?
21. Are MMIO width and ordering preserved?
22. Are side effects preserved?
23. Are timing-visible consequences preserved?
24. Are guest physical addresses separate from CPU MMIO addresses?
25. Does Rigel remain host-independent?
26. Can the harness exercise the reference semantics?
27. Can optimized and reference paths be compared?
28. Does the optimization improve implementation without changing hardware behavior?

---

# 45. Target Architecture

~~~text
                        M68K CPU
                           │
                           ▼
                    Bellatrix / Emu68
                           │
          ┌────────────────┼─────────────────┐
          │                │                 │
          ▼                ▼                 ▼
       caches          fast paths          shadows
                                             │
                                        INT_shadow
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
                        └──┬───┘
                           │
                           ▼
                    Rigel semantics
                           │
           ┌───────────────┼───────────────┐
           │               │               │
           ▼               ▼               ▼
        custom            CIA            other
        hardware        hardware        hardware
           │
           └───────────────┬───────────────┘
                           │
                           ▼
                 authoritative classic
                    hardware state
~~~

For interrupts specifically:

~~~text
                     M68K access
                         │
                         ▼
                       Emu68
                         │
                ┌────────┴────────┐
                │                 │
                ▼                 ▼
          INT_shadow path     generic path
                │                 │
                └────────┬────────┘
                         │
                         ▼
                       Rigel
                         │
                  ┌──────┴──────┐
                  ▼             ▼
               INTENA         INTREQ
                  │             │
                  └──────┬──────┘
                         ▼
                priority resolution
                         │
                         ▼
                     Rigel IPL
                         │
                         ▼
              Bellatrix arbitration
                         │
                         ▼
                       M68K
~~~

The exact optimized path may differ.

The authority relationship may not.

---

# 46. Definition of Done for Rigel API Version 1

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
* optimization misses correctly fall back;
* optimized paths preserve observable Rigel semantics;
* `INT_shadow` has been explicitly reviewed;
* `INT_shadow` cannot become an independent interrupt authority;
* `INTENA` semantics remain Rigel-owned;
* `INTREQ` semantics remain Rigel-owned;
* Rigel-generated interrupt changes cannot silently diverge from shadow state;
* Rigel IPL remains authoritative for the classic interrupt domain;
* Bellatrix does not reconstruct classic IPL from Emu68 shadow state;
* DMA ownership remains unchanged;
* timing ownership remains unchanged;
* Rigel remains host-topology neutral;
* harness and Bellatrix use compatible production semantics;
* optimized and reference paths have behavioral equivalence tests.

For Version 1:

> Stable public source-level API. Binary ABI stability is not implied unless separately documented.

---

# 47. Final Recommendation

The Rigel/Bellatrix/Emu68 relationship should be governed by three independent concepts:

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
~~~

These concepts must not be collapsed into one another.

In particular:

> **Rigel defines what the classic hardware means.**

> **Bellatrix guarantees that supported hardware remains reachable.**

> **Emu68 remains free to optimize how accesses are executed.**

For `INTENA` and `INTREQ`:

~~~text
                  Rigel
                    │
               authoritative
              interrupt semantics
                    │
             ┌──────┴──────┐
             ▼             ▼
          INTENA         INTREQ
             │             │
             └──────┬──────┘
                    ▼
                Rigel IPL
~~~

while:

~~~text
                Emu68
                  │
                  ▼
              INT_shadow
                  │
                  ▼
          optimization / mirror
```

must remain subordinate to that authority.

Therefore the final compatibility rule is:

> **No software should fail because it accessed a Rigel-supported classic hardware address that was missing from a Bellatrix or Emu68 optimization path.**

And the corresponding performance rule is:

> **Satisfying complete Rigel hardware visibility must not require disabling valid Emu68 caches, fast paths, shadows, or specialized handlers.**

The desired architecture is therefore:

~~~text
M68K software
     │
     ▼
Bellatrix / Emu68
     │
     ├── cache
     ├── fast path
     ├── shadow
     │     └── INT_shadow
     ├── specialized handler
     └── generic MMU/Bus fallback
              │
              ▼
       Rigel-defined semantics
              │
              ▼
       classic Amiga hardware
~~~

with one non-negotiable invariant:

> **Optimization may change the path. It may never change which Rigel-supported hardware the M68K CPU can correctly access.**
