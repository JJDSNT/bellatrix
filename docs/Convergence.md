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

> Preserve the existing Rigel execution and hardware boundary. Refine only those public interfaces where the current API exposes implementation details, mixes host and hardware concerns, requires the host to understand Rigel-owned semantics, or permits CPU-visible Rigel hardware to become inaccessible to M68K software.

A critical requirement is:

> **Every M68K CPU-visible classic hardware access supported by Rigel MUST remain functionally reachable through Bellatrix.**

This requirement defines **semantic visibility**, not a mandatory implementation path.

It does **not** require every access to pass through:

* an MMU fault;
* the generic Bellatrix Bus dispatcher;
* a generic Rigel MMIO router;
* the same lookup mechanism;
* the same code path.

Bellatrix and Rigel remain free to use:

* caches;
* fast paths;
* shadows;
* direct dispatch;
* pre-resolved providers;
* lookup tables;
* region coalescing;
* specialized register paths;
* direct mappings where semantically valid;
* other transparent optimizations.

The invariant is:

> **Complete address visibility is mandatory. The access path is not.**

Conceptually:

~~~text
M68K access to X
        │
        ▼
Does Rigel support X?
        │
       yes
        │
        ▼
X MUST remain functionally reachable
        │
        ▼
correct Rigel-visible semantics
~~~

The following must never occur:

~~~text
M68K accesses X
        │
        ▼
Rigel supports X
        │
        ▼
Bellatrix integration omitted X
        │
        ▼
unmapped / incorrect / invisible
~~~

The primary areas requiring attention are:

* public API organization;
* separation of hardware configuration from host services;
* opaque host context semantics;
* explicit guest-physical memory semantics;
* canonical M68K-visible MMIO semantics;
* authoritative Rigel CPU-visible address coverage;
* complete functional reachability of every CPU-visible Rigel address;
* optimization-safe host integration;
* MMU and Bus policy without imposing mandatory slow paths;
* MMIO width, alignment, ordering, side-effect, and result semantics;
* removal of direct host access to Rigel internals;
* explicit lifecycle and reset semantics;
* explicit execution/concurrency contract without constraining host topology;
* preservation of Rigel's authoritative chipset timeline;
* preservation of Rigel IPL ownership;
* preservation of DMA and Chip RAM ownership rules;
* formalization of video output as host-consumable classic chipset output;
* separation of integration APIs from diagnostic and advanced APIs;
* preservation of existing observable behavior during API refinement.

---

# 2. Classification Model

Every proposed API change should first be classified into one of four categories:

~~~text
PRESERVE

    The current implementation and host boundary are already
    architecturally correct.

FORMALIZE

    The current behavior is already correct, but its public
    contract is implicit or insufficiently documented.

CHANGE

    The current public API requires host/chipset responsibility
    leakage, mixes unrelated concerns, exposes an unsuitable
    host-facing abstraction, or allows CPU-visible Rigel
    hardware to become functionally unreachable.

INTERNALIZE

    The functionality may remain useful internally, for tests,
    inspection, or tooling, but should not remain part of the
    normal host integration contract.
~~~

The existence of a topic in this document does not imply that the current Rigel implementation is deficient in that area.

---

# 3. Guiding Principles

The convergence work should follow these rules:

> Preserve working Rigel semantics.

> Refine the API boundary rather than rewriting the chipset.

> Do not treat already-correct host/Rigel ownership as a migration task.

> A host-side boundary violation does not automatically justify expanding the Rigel API.

> The Rigel API defines operation semantics and ordering requirements. The host decides where and how those operations execute.

> Rigel must support host freedom without becoming aware of host execution topology.

> Bellatrix must not interpret classic hardware semantics that belong to Rigel.

> Rigel must not acquire Bellatrix, Emu68, Raspberry Pi, AROS, VC4, scheduler, or core-specific knowledge.

> **Every CPU-visible classic hardware address supported by Rigel must remain semantically reachable to M68K software.**

> **Bellatrix must never accidentally expose only a subset of Rigel's CPU-visible hardware.**

> **Visibility is a semantic invariant, not an implementation-shape invariant.**

> No CPU-visible Rigel address may become inaccessible merely because the optimized execution path bypasses the generic dispatcher.

> Caches, fast paths, shadows, direct paths, and other optimizations are valid provided they preserve observable classic hardware behavior.

> Rigel should remain authoritative for what CPU-visible classic hardware it implements.

---

# 4. Existing Rigel Architecture

The current Rigel implementation already contains the major classic hardware domains expected from the compatibility component.

Conceptually:

~~~text
Rigel
│
├── public API
│
├── chipset composition
│   ├── Agnus
│   ├── Denise
│   ├── Paula
│   ├── CIA
│   └── related devices
│
├── hardware domains
│   ├── beam
│   ├── DMA
│   ├── Copper
│   ├── Blitter
│   ├── interrupts
│   ├── audio
│   ├── disk
│   ├── serial
│   └── input
│
├── deterministic timing
├── bus observation
├── classic video generation
└── harness / tests
~~~

This organization should remain.

Hardware domains represent ownership of chipset state and behavior.

They must not be confused with host execution threads, ARM cores, queues, schedulers, MMU implementation details, dispatcher topology, or Bellatrix runtime policy.

---

# 5. What Should Be Preserved

## 5.1 Host-independent chipset implementation

Rigel should remain independent from:

* Bellatrix;
* Raspberry Pi hardware;
* Emu68 internals;
* VC4;
* BCM interrupt controllers;
* USB;
* Bluetooth;
* AROS;
* host scheduler topology;
* host core numbering;
* host synchronization primitives;
* host MMU implementation details;
* host dispatch implementation details.

No Bellatrix-specific dependency should enter the Rigel core.

---

## 5.2 Deterministic execution model

Given identical defined configuration, guest memory, MMIO accesses, input, and execution progress, Rigel should produce identical defined hardware state transitions and outputs.

Host wall-clock timing remains outside chipset correctness.

Host optimization strategy must not change defined observable Rigel behavior.

---

## 5.3 Temporal model

Rigel owns the authoritative classic chipset timeline.

~~~text
Rigel
  │
  ▼
next deadline
  │
  ▼
Host executes CPU
  │
  ▼
execution progress
  │
  ▼
Rigel step/advance
~~~

This ownership should remain unchanged.

---

## 5.4 Interrupt ownership

Rigel owns:

~~~text
INTREQ
INTENA
classic interrupt sources
classic priority resolution
Rigel IPL
~~~

Bellatrix consumes the resulting IPL.

---

## 5.5 DMA ownership

Rigel owns:

* Agnus DMA semantics;
* Paula DMA semantics;
* Copper and Blitter memory behavior;
* chipset-generated address interpretation;
* classic Chip RAM visibility rules.

Bellatrix provides memory services but must not reproduce classic DMA semantics.

---

## 5.6 Host-provided memory

Rigel operates on host-provided guest memory.

Rigel does not allocate Bellatrix guest physical memory.

---

## 5.7 Host-controlled execution topology

Bellatrix may execute Rigel:

~~~text
same core

different ARM core

worker thread

queue consumer

serialized remote context
~~~

without changing Rigel semantics.

---

## 5.8 Host-controlled access optimization

Bellatrix may optimize access delivery using:

~~~text
generic dispatcher

cached provider lookup

fast path

shadow

direct provider call

specialized path

region coalescing

validated direct mapping
~~~

provided the optimization preserves the same defined observable hardware semantics.

No optimization may make a valid Rigel-supported CPU access disappear.

---

## 5.9 Standalone harness

The standalone harness remains a first-class consumer of the production API.

~~~text
Bellatrix ──┐
            ├──► public Rigel API ──► librigel
Harness ────┘
~~~

---

# 6. Behavioral Preservation

Before changing the public boundary, establish a behavioral baseline covering:

* CPU-visible address decode;
* MMIO accesses;
* MMIO side effects;
* timing;
* deadlines;
* IPL transitions;
* DMA;
* reset;
* video;
* audio;
* bus behavior where relevant.

The baseline should verify semantic behavior rather than requiring identical internal execution paths.

For example:

~~~text
generic dispatcher path
        │
        ▼
observable result X


optimized fast path
        │
        ▼
observable result X
~~~

is valid.

API refinement or optimization must not silently change classic hardware behavior.

---

# 7. Public API Boundary

The public API should be conceptually divided into:

~~~text
Rigel API
   │
   ├── Host Integration API
   │      lifecycle
   │      CPU-visible coverage
   │      MMIO
   │      progress
   │      deadlines
   │      IPL
   │      guest memory
   │      video
   │      audio
   │      input
   │
   └── Advanced / Inspection API
          bus inspection
          beam inspection
          snapshots
          diagnostics
          testing controls
~~~

Bellatrix should normally depend only on the Host Integration API.

---

# 8. Opaque Rigel Instance

The primary Rigel object should remain opaque:

~~~c
struct rigel;
~~~

Bellatrix must not directly access internal chipset structures.

---

# 9. Configuration and Host Services

Hardware configuration and host services should remain distinct.

Conceptually:

~~~c
struct rigel_config {
    enum rigel_chipset chipset;
    enum rigel_video_standard video_standard;
    uint32_t chip_ram_size;
};

struct rigel_host_ops {
    uint8_t  (*mem_read8)(void *ctx, uint32_t addr);
    uint16_t (*mem_read16)(void *ctx, uint32_t addr);
    uint32_t (*mem_read32)(void *ctx, uint32_t addr);

    void (*mem_write8)(void *ctx, uint32_t addr, uint8_t value);
    void (*mem_write16)(void *ctx, uint32_t addr, uint16_t value);
    void (*mem_write32)(void *ctx, uint32_t addr, uint32_t value);

    void (*log)(void *ctx, int level, const char *message);
};
~~~

Exact signatures are not normative.

---

# 10. Host Context

The host context is opaque.

Rigel receives it from the host and returns it unchanged to host callbacks.

Rigel never interprets its contents.

---

# 11. Chip RAM Ownership

Chip RAM configuration describes chipset-visible memory topology.

It does not imply allocation ownership.

~~~text
Bellatrix
    │
    ├── allocates
    ├── maps
    └── backs guest memory
            │
            ▼
          Rigel
            │
            └── applies classic
                Chip RAM semantics
~~~

This is separate from CPU-visible Rigel MMIO coverage.

---

# 12. Memory Address Semantics

Addresses passed to host memory callbacks are guest physical addresses.

~~~text
chipset-generated address
        │
        ▼
Rigel classic address rules
        │
        ▼
guest physical address
        │
        ▼
host memory callback
~~~

These addresses must not be confused with CPU-visible MMIO addresses.

---

# 13. Host Memory as Coherency Boundary

Rigel DMA writes pass through the host memory interface.

The host may perform required coherency actions, including Emu68 JIT invalidation where necessary.

Rigel remains unaware of JIT internals.

---

# 14. CPU-Visible Rigel Hardware

Rigel must define the CPU-visible classic hardware interface that it supports.

Conceptually:

~~~text
Rigel-supported CPU-visible interface
                    │
                    ▼
          host integration knowledge
                    │
                    ▼
       complete functional reachability
~~~

The essential invariant is:

> If Rigel supports the defined semantics of a CPU-visible M68K hardware access, Bellatrix must provide a valid execution path for that access.

This does not require a one-address-per-entry representation.

It does not require every address to fault.

It does not require every access to pass through a generic dispatcher.

It requires only that no supported access become invisible.

---

# 15. Semantic Reachability

Define:

~~~text
R = set of CPU-visible classic hardware accesses
    whose semantics are supported by Rigel

H = set of CPU-visible accesses that Bellatrix
    can correctly deliver to Rigel-equivalent semantics
~~~

The required invariant is:

~~~text
R ⊆ H
~~~

This is the fundamental address-visibility requirement.

It deliberately does not define:

~~~text
R = MMU faults

R = Bellatrix Bus calls

R = generic dispatcher calls
~~~

because those are implementation mechanisms.

---

# 16. Visibility Versus Access Path

The following distinction is normative.

~~~text
VISIBILITY
    mandatory

SEMANTIC REACHABILITY
    mandatory

OBSERVABLE HARDWARE BEHAVIOR
    mandatory


GENERIC DISPATCH
    implementation choice

MMU FAULT
    implementation choice

CACHE
    implementation choice

FAST PATH
    implementation choice

SHADOW
    implementation choice

DIRECT PATH
    implementation choice
~~~

Therefore:

> **Complete address visibility is mandatory; the access path is not.**

---

# 17. Canonical MMIO Boundary

Rigel should expose a canonical host-facing M68K-visible MMIO transaction interface capable of representing the complete CPU-visible classic hardware semantics supported by Rigel.

Conceptually:

~~~text
M68K-visible transaction
        │
        ├── address
        ├── width
        ├── direction
        └── value
        │
        ▼
canonical Rigel MMIO
        │
        ▼
classic hardware semantics
~~~

Example:

~~~text
address = 0x00DFF096
width   = 16
write   = 0x8200
~~~

Bellatrix must not need to convert this into:

~~~text
DMACON
custom register 0x096
Agnus-specific operation
~~~

before entering the Rigel compatibility domain.

---

# 18. Canonical MMIO Is the Semantic Reference Path

Canonical MMIO defines the normal public semantic boundary.

It does not imply that every optimized host access must physically execute through the generic canonical dispatcher.

Conceptually:

~~~text
                    M68K access
                        │
                        ▼
                host access resolution
                        │
       ┌────────────────┼────────────────┐
       │                │                │
       ▼                ▼                ▼
 generic MMIO       cached path       fast path
       │                │                │
       │                ▼                │
       │             shadow              │
       │                │                │
       └────────────────┼────────────────┘
                        ▼
              Rigel-defined semantics
~~~

All paths must remain semantically equivalent where the contract requires equivalence.

---

# 19. No Hidden CPU-Visible Hardware

A CPU-visible Rigel capability must not exist only behind:

~~~text
internal subsystem API

test API

debug API

private helper
~~~

if doing so makes that hardware inaccessible to M68K software hosted by Bellatrix.

Internal helpers are valid.

Internal-only CPU-visible reachability is not.

---

# 20. Authoritative Coverage Description

Rigel should provide enough host-neutral information for Bellatrix to guarantee complete CPU-visible reachability.

A possible representation is:

~~~c
struct rigel_mmio_region {
    uint32_t start;
    uint32_t end;
    uint32_t flags;
};
~~~

The exact representation is not normative.

The region description may be:

* fine-grained;
* coarse-grained;
* coalesced;
* generated;
* static;
* optimized.

Its purpose is not to prescribe the dispatch implementation.

Its purpose is to prevent Bellatrix from accidentally omitting CPU-visible Rigel hardware.

---

# 21. Coverage Description Is Not a Mandatory Slow Path

The authoritative coverage description tells the host:

~~~text
which CPU-visible address space must remain valid
~~~

It does not tell the host:

~~~text
how every access must be executed
~~~

For example:

~~~text
Rigel coverage
      │
      ▼
Bellatrix policy
      │
      ├── generic Bus route
      ├── cached provider
      ├── fast path
      ├── shadow path
      └── other validated optimization
~~~

All are valid.

---

# 22. Region Holes and Subdecode

Rigel may advertise larger regions containing internal holes.

For example:

~~~text
advertised region:

1 2 3 4 5 6 7 8
      │
      ▼
Rigel subdecode:

1 register
2 register
3 hole
4 register
5 open bus
6 register
7 ignored write
8 register
~~~

Bellatrix does not need to understand those distinctions.

If an access reaches Rigel's canonical path, Rigel determines the classic behavior.

An optimized path may avoid generic subdecode only if it preserves equivalent observable behavior.

---

# 23. Bellatrix Knows Ownership, Not Meaning

Bellatrix may know:

~~~text
this address belongs to the Rigel compatibility domain
~~~

Bellatrix should not need to know:

~~~text
this is DMACON

this is INTENA

this is CIAA PRA

this starts Paula DMA
~~~

The normal responsibility boundary remains:

~~~text
Bellatrix
    │
    │ resolves ownership / access path
    ▼
Rigel
    │
    │ defines classic hardware meaning
    ▼
chipset implementation
~~~

---

# 24. MMU Policy

The Emu68 MMU is one mechanism Bellatrix may use to make Rigel regions interceptable.

Conceptually:

~~~text
Rigel coverage
      │
      ▼
Bellatrix machine policy
      │
      ▼
Emu68 MMU map/unmap facilities
      │
      ▼
fault/interception where required
~~~

Bellatrix should reuse existing Emu68 MMU mechanisms wherever sufficient.

Bellatrix should not create a parallel MMU implementation.

However:

> MMU faulting is not itself part of the Rigel semantic contract.

An optimized path may avoid a fault where correctness is preserved.

---

# 25. Bellatrix Bus

The Bellatrix Bus provides the generic routing mechanism for accesses that require generic host dispatch.

Conceptually:

~~~text
M68K access
    │
    ▼
MMU/fault or other generic entry
    │
    ▼
Bellatrix Bus
    │
    ▼
Rigel
~~~

But the Bus is not required to be the physical execution path for every optimized Rigel access.

The requirement is:

> Any access that falls back to the generic Bellatrix Bus path must remain routable to the correct Rigel provider.

---

# 26. Generic Fallback Must Be Complete

Optimizations may exist above the generic dispatcher.

However, the generic fallback should remain capable of resolving the complete Rigel coverage for which it is responsible.

Conceptually:

~~~text
M68K access
     │
     ▼
optimized path available?
     │
    / \
  yes  no
  │     │
  ▼     ▼
fast   generic
path   dispatcher
  │     │
  └──┬──┘
     ▼
correct semantics
~~~

This ensures that an optimization is not required merely to make an otherwise invisible address work.

---

# 27. Fast Paths

Fast paths are explicitly permitted.

A fast path may:

* avoid repeated provider lookup;
* avoid generic Bus traversal;
* use pre-decoded routing metadata;
* call a specialized Rigel entry point;
* use validated direct access;
* exploit stable region ownership.

A fast path is valid if:

~~~text
observable(fast_path(X))
        =
observable(reference_semantics(X))
~~~

for all behavior required by the contract.

---

# 28. Caches

Caching routing information is explicitly permitted.

Examples include:

~~~text
address → provider cache

region → provider cache

decoded access metadata cache

JIT block → Rigel access metadata

pre-resolved MMIO target
~~~

Caching must not incorrectly survive changes that invalidate the cached assumptions.

Cache invalidation belongs to the component owning the optimization.

Rigel must not become aware of Emu68 JIT cache internals.

---

# 29. Shadows

Shadows are explicitly permitted where they preserve required semantics.

A shadow may represent:

* cached host-visible state;
* mirrored state;
* optimized read state;
* precomputed routing state;
* another validated representation.

But:

> A shadow is an optimization or representation mechanism, not a second independent authority for classic hardware semantics.

Where Rigel owns the authoritative classic hardware state, the shadow must remain coherent with Rigel according to the defined contract.

A shadow must never make the original CPU-visible address inaccessible.

---

# 30. Direct Paths

Direct paths are permitted where the hardware semantics make them safe.

Conceptually:

~~~text
M68K access
     │
     ▼
validated direct path
     │
     ▼
Rigel-equivalent semantics
~~~

A direct path must not bypass required:

* side effects;
* ordering;
* timing visibility;
* interrupt changes;
* DMA interactions;
* read semantics;
* write semantics.

---

# 31. Optimization Must Be Transparent

The correctness test for an optimization is behavioral.

Conceptually:

~~~text
reference path
     │
     ▼
observable behavior
     │
     =

optimized path
     │
     ▼
observable behavior
~~~

Implementation identity is not required.

Semantic equivalence is.

---

# 32. MMIO Width, Alignment, and Ordering

Before API Version 1, the MMIO contract must define:

* supported widths;
* alignment;
* misaligned accesses;
* wider access decomposition;
* operation ordering;
* side effects;
* unsupported-width behavior.

These rules apply equally to generic and optimized paths.

An optimization must not change transaction semantics merely because it avoids generic dispatch.

---

# 33. MMIO Is Observable

The host or execution engine must not freely:

* eliminate;
* duplicate;
* cache values;
* reorder;
* combine;
* split;

Rigel MMIO transactions unless the contract explicitly permits that transformation.

Routing metadata may be cached.

Observable hardware transactions may not be incorrectly optimized away.

---

# 34. MMIO Results

The canonical MMIO interface should distinguish concepts equivalent to:

~~~text
HANDLED

UNHANDLED

INVALID_ACCESS

HOST_FAILURE
~~~

For an access that Rigel defines as supported CPU-visible behavior, failure solely because Bellatrix did not provide a route is an integration error.

---

# 35. Address Namespace Separation

The public contract must distinguish:

~~~text
M68K CPU-visible MMIO address

chipset-generated DMA address

guest physical address

host pointer
~~~

These namespaces must never be conflated.

---

# 36. Temporal Model

Rigel remains authoritative for chipset time.

~~~text
host CPU progress
      │
      ▼
Rigel step
      │
      ▼
chipset timeline
~~~

The host must not introduce a second authoritative classic chipset clock.

Optimized MMIO paths must preserve whatever timing interaction the Rigel contract requires.

---

# 37. IPL

Rigel continues to expose resolved classic IPL.

~~~c
unsigned rigel_get_ipl(const struct rigel *rigel);
~~~

Bellatrix consumes it as one source in its interrupt arbitration.

---

# 38. INTREQ and INTENA

Detailed interrupt state may remain available for diagnostics and testing.

Bellatrix must not reconstruct Rigel IPL from those registers.

A shadow of interrupt-related state, if used for optimization, must not become an independent interrupt authority.

---

# 39. DMA

DMA remains separate from CPU-visible MMIO.

~~~text
CPU MMIO
   │
   ▼
host-selected access path
   │
   ▼
Rigel


Rigel DMA
   │
   ▼
guest physical memory API
~~~

DMA is not reverse Bellatrix Bus traffic.

---

# 40. Video

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
   │
   ▼
Bellatrix presentation
~~~

Rigel remains independent of RTG, P96, VC4, and AROS native graphics.

---

# 41. Audio and Input

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
Bellatrix adaptation
    │
    ▼
Rigel classic input state
~~~

---

# 42. Concurrency Contract

Rigel should define serialization and reentrancy requirements without defining host topology.

Correct principle:

> A Rigel instance is non-concurrent unless otherwise documented. The host is responsible for serialization.

Rigel must remain usable under same-core or cross-core execution without API changes.

Optimized access paths remain subject to the same ordering and serialization requirements.

---

# 43. Bellatrix Adapter Responsibilities

The Bellatrix adapter should:

~~~text
create/configure Rigel

obtain enough information to guarantee
complete Rigel CPU-visible reachability

configure Emu68 MMU interception
where interception is required

register generic Bus fallback coverage

forward canonical MMIO

support validated optimized paths

provide guest memory

report execution progress

consume deadlines

obtain Rigel IPL

consume video/audio output

adapt input
~~~

It must not contain:

~~~text
partial manually maintained Rigel coverage

independent Amiga hardware semantics

Copper semantics

Blitter semantics

Paula semantics

CIA semantics

INTREQ/INTENA authority

Agnus DMA rules
~~~

---

# 44. Bellatrix Access Architecture

The intended access architecture is:

~~~text
                         M68K access
                              │
                              ▼
                     Bellatrix access policy
                              │
          ┌───────────────────┼───────────────────┐
          │                   │                   │
          ▼                   ▼                   ▼
       fast path            cache              shadow
          │                   │                   │
          └─────────────┬─────┴─────────────┬─────┘
                        │                   │
                        ▼                   ▼
                 resolved access      generic fallback
                                            │
                                            ▼
                                      MMU / fault
                                            │
                                            ▼
                                      Bellatrix Bus
                                            │
                                            ▼
                                      canonical MMIO
                        │                   │
                        └─────────┬─────────┘
                                  ▼
                         Rigel semantics
~~~

Not every access must traverse every layer.

Every supported access must reach the correct semantics.

---

# 45. Required Semantic Invariant

Define:

~~~text
R = CPU-visible accesses supported by Rigel

H = CPU-visible accesses for which Bellatrix
    provides a correct execution path
~~~

Required:

~~~text
R ⊆ H
~~~

This is the normative coverage invariant.

No equality between:

~~~text
Rigel coverage

MMU faults

Bus routes

generic MMIO calls
~~~

is required.

Those sets may differ because optimizations may bypass generic mechanisms.

---

# 46. Reference Path and Optimized Paths

The architecture should maintain a conceptual reference path:

~~~text
M68K access
    │
    ▼
generic host resolution
    │
    ▼
canonical Rigel MMIO
    │
    ▼
Rigel semantics
~~~

Optimized paths may replace portions of that execution:

~~~text
M68K access
    │
    ├──► fast path ───────┐
    │                     │
    ├──► cached path ─────┤
    │                     ├──► equivalent semantics
    ├──► shadow path ─────┤
    │                     │
    └──► generic path ────┘
~~~

The reference path defines semantic expectations.

It does not define mandatory runtime topology.

---

# 47. Example of the Visibility Requirement

Assume Rigel supports CPU-visible accesses to:

~~~text
1 2 3 4 5 6 7 8 9 10
~~~

Bellatrix must ensure that software can correctly access:

~~~text
1 2 3 4 5 6 7 8 9 10
~~~

But the implementation may look like:

~~~text
1 ──► fast path

2 ──► cache ──► Rigel

3 ──► generic Bus ──► Rigel

4 ──► shadow

5 ──► generic Bus ──► Rigel

6 ──► direct Rigel path

7 ──► cache ──► Rigel

8 ──► generic Bus ──► Rigel

9 ──► fast path

10 ─► generic Bus ──► Rigel
~~~

provided:

~~~text
observable semantics
        =
Rigel-defined semantics
~~~

for every required access.

The forbidden result is:

~~~text
1 2 3 4 5   7 8 9 10
          ^
          │
      6 invisible
~~~

The problem is not that `6` avoided the dispatcher.

The problem is if `6` has no correct CPU-visible path at all.

---

# 48. Migration Strategy

Recommended sequence:

~~~text
1. Capture behavioral baseline

2. Inventory Rigel CPU-visible behavior

3. Establish authoritative Rigel coverage information

4. Establish canonical MMIO semantics

5. Verify complete functional reachability

6. Establish generic Bellatrix fallback routing

7. Integrate with existing Emu68 MMU mechanisms

8. Validate that no supported Rigel access is omitted

9. Remove manually selected incomplete coverage

10. Formalize cache / fast-path / shadow rules

11. Validate optimized paths against reference behavior

12. Formalize memory, timing, IPL,
    lifecycle and concurrency

13. Migrate harness

14. Migrate Bellatrix

15. Freeze API Version 1

16. Optimize further only while preserving
    semantic equivalence
~~~

---

# 49. Conformance Tests

## Complete CPU-visible reachability

Every CPU-visible hardware access supported by Rigel must have a valid Bellatrix execution path.

## No invisible supported address

A test must fail if:

~~~text
Rigel supports X
        │
        ▼
M68K software cannot correctly access X
~~~

## Canonical reference behavior

The canonical MMIO path must provide the reference host-facing semantics for Rigel CPU-visible hardware.

## Generic fallback completeness

Addresses relying on generic dispatch must remain routable through the generic Bellatrix path.

## Optimization equivalence

Where both reference and optimized paths exist:

~~~text
observable(reference)
        =
observable(optimized)
~~~

for the behavior covered by the contract.

## Cache correctness

Cached routing must not make previously valid accesses stale, invisible, or incorrectly routed.

## Fast-path correctness

Fast paths must preserve required side effects, ordering, timing interaction, and hardware semantics.

## Shadow correctness

Shadows must remain coherent with authoritative Rigel state according to their defined contract.

## MMU independence

A valid optimization must not fail conformance merely because it avoids an MMU fault.

## Bus independence

A valid optimization must not fail conformance merely because it avoids generic Bellatrix Bus traversal.

## No host-side semantic duplication

Bellatrix must not become an independent implementation of classic Amiga hardware merely to optimize dispatch.

---

# 50. Review Checklist

Every relevant patch should answer:

1. Does Rigel support this CPU-visible access?
2. Can M68K software still perform it correctly?
3. Could the address become invisible because Bellatrix omitted it?
4. Does a generic fallback exist where required?
5. Is the optimization preserving observable semantics?
6. Is an MMU fault genuinely required, or merely one implementation mechanism?
7. Is generic Bus traversal genuinely required, or merely one implementation mechanism?
8. Can provider lookup safely be cached?
9. Does the cache have correct invalidation semantics?
10. Does a fast path preserve side effects?
11. Does a fast path preserve ordering?
12. Does a fast path preserve required timing interaction?
13. Is a shadow coherent with Rigel's authoritative state?
14. Has a shadow accidentally become a second hardware authority?
15. Is a CPU-visible address accessible only through a private API?
16. Is Bellatrix maintaining a manually selected incomplete subset?
17. Is Bellatrix interpreting classic hardware semantics unnecessarily?
18. Are region holes still given correct Rigel-defined behavior?
19. Are M68K MMIO and guest physical memory kept distinct?
20. Does the optimization preserve IPL behavior?
21. Does the optimization preserve DMA interaction?
22. Does the optimization preserve deterministic behavior?
23. Can the harness exercise the reference semantics?
24. Can optimized and reference paths be compared?
25. Does the change preserve host-topology neutrality?

---

# 51. Target Architecture

~~~text
                         Rigel
                           │
                  CPU-visible hardware
                     semantics/coverage
                           │
                           ▼
                       Bellatrix
                           │
                   host access policy
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
        ▼                  ▼                  ▼
     fast path           cache              shadow
        │                  │                  │
        └───────────┬──────┴───────────┬──────┘
                    │                  │
                    │           generic fallback
                    │                  │
                    │          ┌───────┴───────┐
                    │          ▼               ▼
                    │      Emu68 MMU      Bellatrix Bus
                    │          │               │
                    │          └───────┬───────┘
                    │                  ▼
                    │          canonical MMIO
                    │                  │
                    └──────────┬───────┘
                               ▼
                       Rigel-defined behavior
~~~

The critical property is:

~~~text
Rigel supports X
       │
       ▼
Bellatrix knows X must remain valid
       │
       ▼
host chooses an access mechanism
       │
       ├── generic
       ├── cached
       ├── fast
       ├── shadow
       └── direct
       │
       ▼
correct observable semantics
~~~

There must be no requirement that every access traverse the same path.

There must be a requirement that every supported access remains valid.

---

# 52. Definition of Done for Rigel API Version 1

API Version 1 should not be frozen until:

* Rigel has an authoritative definition or discoverable description sufficient to identify its CPU-visible classic hardware coverage;
* every CPU-visible access supported by Rigel remains functionally reachable through Bellatrix;
* canonical MMIO provides a complete semantic reference interface;
* Bellatrix does not maintain a manually selected incomplete subset of Rigel hardware;
* the generic fallback can route all accesses assigned to it;
* Emu68 MMU mechanisms are reused where interception is required;
* MMU faults are not incorrectly treated as mandatory for every Rigel access;
* Bellatrix Bus traversal is not incorrectly treated as mandatory for every Rigel access;
* caches are explicitly permitted;
* fast paths are explicitly permitted;
* shadows are explicitly permitted;
* direct paths are explicitly permitted where semantically valid;
* optimized paths preserve required observable behavior;
* shadows do not become independent classic hardware authorities;
* no CPU-visible Rigel hardware exists only behind an inaccessible private path;
* Bellatrix does not unnecessarily interpret classic register semantics;
* guest-memory address semantics remain explicit;
* Rigel remains authoritative for classic timing and interrupts;
* DMA ownership remains unchanged;
* Rigel remains host-topology neutral;
* harness and Bellatrix use the same production semantic interface;
* behavioral equivalence has been validated.

For Version 1:

> Stable public source-level API. Binary ABI stability is not implied unless separately documented.

---

# 53. Relationship to the Integration Specification

The authority hierarchy remains:

~~~text
Bellatrix.md
      │
      ▼
architecture
      │
      ▼
Rigel_integration.md
      │
      ▼
cross-boundary behavioral contract
      │
      ▼
Rigel API Convergence Plan
      │
      ▼
API refinement
      │
      ▼
public Rigel headers
      │
      ▼
librigel
~~~

This document does not redefine `Rigel_integration.md`.

Its purpose is to identify which parts of the current Rigel boundary should be:

~~~text
preserved

formalized

changed

internalized
~~~

while ensuring complete CPU-visible compatibility without preventing future optimization.

---

# 54. Final Recommendation

The existing Rigel implementation should remain the foundation of `librigel`.

Its working:

* chipset implementation;
* deterministic execution model;
* temporal model;
* interrupt model;
* DMA behavior;
* bus infrastructure;
* Denise renderer;
* chunky video output;
* harness;

should remain intact wherever they already satisfy the required contract.

The primary refinement is the public host boundary.

The central CPU-visible invariant is:

> **Every M68K CPU-visible classic hardware access supported by Rigel must remain functionally reachable through Bellatrix.**

The central optimization invariant is:

> **Complete address visibility is mandatory; the access path is not.**

Therefore the architecture explicitly permits:

~~~text
generic dispatcher

MMU interception

cached provider lookup

fast paths

shadows

direct paths

region coalescing

specialized dispatch

future transparent optimizations
~~~

provided that:

~~~text
observable optimized behavior
            =
required Rigel-defined behavior
~~~

The intended relationship is:

~~~text
Rigel
    defines classic hardware semantics
    and supported CPU-visible behavior

Bellatrix
    guarantees that supported behavior
    remains reachable

Bellatrix / Emu68
    choose how each access is transported
    and optimized

Rigel
    remains authoritative for classic
    hardware meaning
~~~

The architecture must therefore avoid both extremes.

This is too restrictive:

~~~text
every Rigel access
      MUST
fault through MMU
      MUST
enter generic Bus
      MUST
enter generic dispatcher
~~~

because it unnecessarily prevents optimization.

This is incorrect:

~~~text
Rigel supports address X

but Bellatrix has no path for X
~~~

because it breaks compatibility.

The desired model is:

~~~text
                         M68K
                           │
                     access to X
                           │
                           ▼
                  Bellatrix / Emu68
                           │
                    choose best path
                           │
          ┌────────────────┼────────────────┐
          │                │                │
        cache           fast path         generic
          │                │                │
        shadow          direct path      MMU / Bus
          │                │                │
          └────────────────┼────────────────┘
                           ▼
                   correct Rigel semantics
~~~

Thus the final rule for Rigel API Version 1 is:

> **Rigel defines what classic CPU-visible hardware behavior exists. Bellatrix guarantees that none of that behavior becomes inaccessible to M68K software, while remaining free to use caches, fast paths, shadows, direct paths, MMU interception, generic dispatch, or future optimizations to implement that visibility.**

Or, in its shortest form:

> **Semantic reachability is mandatory. Dispatch topology is an optimization choice.**
