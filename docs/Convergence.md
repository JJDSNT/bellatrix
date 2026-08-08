# Rigel API Convergence Plan

## Aligning the Existing Rigel Implementation with the Bellatrix / Rigel Integration Specification

**Status:** Proposed Refactoring Baseline  
**Target:** Rigel API Version 1  
**Related specification:** `Bellatrix/docs/Rigel_integration.md`

---

# 1. Purpose

This document defines the recommended evolution of the existing Rigel implementation so that its public integration boundary conforms cleanly to the architecture established by:

~~~text
Bellatrix.md
        │
        ▼
Rigel_integration.md
        │
        ▼
Rigel public API
        │
        ▼
librigel implementation
~~~

Rigel already implements most of the architectural concepts required by the Bellatrix/Rigel integration model.

The objective is therefore **not to rewrite Rigel**.

The objective is to converge the existing implementation toward a smaller, clearer, and explicitly host-independent integration contract.

The primary work should concentrate on:

* public API organization;
* separation of hardware configuration from host services;
* canonical MMIO dispatch;
* explicit MMIO width, alignment, ordering, side-effect, and result semantics;
* explicit guest-physical memory semantics;
* lifecycle encapsulation;
* explicit reset semantics;
* preservation of Rigel's authoritative chipset timeline;
* separation of integration APIs from diagnostic and advanced APIs;
* preservation of existing observable behavior during refactoring;
* removal of unnecessary implementation details from the Bellatrix-facing boundary.

The existing chipset implementation, timing model, interrupt model, DMA model, bus model, and harness should be preserved wherever they already satisfy the architectural contract.

---

# 2. Guiding Principle

The migration should follow one rule:

> Preserve working Rigel semantics. Refactor the boundary around them.

The desired transformation is therefore:

~~~text
Current Rigel
      │
      ├── existing chipset implementation
      ├── existing timing model
      ├── existing interrupt model
      ├── existing DMA model
      ├── existing bus model
      ├── existing harness
      └── broad public API
              │
              ▼
        API convergence
              │
              ├── minimal host integration API
              ├── explicit host operations
              ├── canonical MMIO boundary
              ├── explicit memory semantics
              ├── encapsulated lifecycle
              └── separate advanced/debug APIs
                      │
                      ▼
             candidate public API
                      │
              ┌───────┴───────┐
              │               │
           Harness         Bellatrix
              │               │
              └───────┬───────┘
                      │
               integration review
                      │
                      ▼
                  librigel v1
~~~

The implementation should not be reorganized merely to resemble the specification structurally.

Changes should be made only where they improve or enforce the architectural boundary.

---

# 3. Existing Rigel Architecture

The current Rigel implementation already contains the major subsystems expected from the classic Amiga compatibility component.

Conceptually:

~~~text
Rigel
│
├── public API
│
├── chipset composition
│
│   ├── Agnus
│   ├── Denise
│   ├── Paula
│   ├── CIA
│   └── related devices
│
├── hardware domains
│
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
├── timing
│
├── bus observation
│
└── harness / tests
~~~

This basic organization should remain.

In particular, hardware domains should continue to represent ownership of chipset state and behavior rather than host execution threads.

Rigel should remain responsible for classic hardware semantics regardless of where or how the host executes the library.

---

# 4. What Should Be Preserved

The convergence work should begin by identifying functionality that already matches the new architecture.

The following concepts should be preserved unless implementation evidence demonstrates a concrete problem.

## 4.1 Host-independent chipset implementation

Rigel should remain independent from:

* Bellatrix;
* Raspberry Pi hardware;
* Emu68 internals;
* VC4;
* BCM interrupt controllers;
* USB;
* Bluetooth;
* AROS.

No Bellatrix-specific dependency should be introduced into the Rigel core during integration work.

---

## 4.2 Deterministic execution model

Rigel's deterministic execution model should remain fundamental.

Given identical defined:

* configuration;
* guest memory;
* MMIO accesses;
* input;
* execution progress;

Rigel should produce identical defined hardware state transitions and outputs.

Host wall-clock timing must remain outside chipset correctness.

---

## 4.3 Deadline-based timing

The existing temporal relationship should be preserved and formalized.

Conceptually:

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
Rigel advances
  │
  ▼
new deadline
~~~

This already corresponds closely to the Bellatrix/Rigel integration architecture.

The timing implementation should therefore be adapted rather than replaced.

---

## 4.4 Rigel interrupt ownership

Rigel should continue to own:

~~~text
INTREQ
INTENA
classic interrupt sources
classic priority resolution
Rigel IPL
~~~

Bellatrix should consume the resulting IPL.

The internal interrupt implementation should not be moved into the Bellatrix adapter.

---

## 4.5 Host-provided memory

Rigel should continue to operate on memory supplied by the host.

Rigel must not become the allocator of Bellatrix guest physical memory.

The current callback-based approach is compatible with the intended architecture and can remain the initial memory backend.

---

## 4.6 Standalone harness

The existing harness is architecturally important and should remain a first-class consumer of `librigel`.

The harness should use the same production API and implementation used by Bellatrix.

There must not be:

~~~text
Rigel for Bellatrix

and separately

simplified Rigel for tests
~~~

The relationship must remain:

~~~text
Bellatrix ──┐
            │
            ├──► public Rigel API ──► librigel
            │
Harness ────┘
~~~

---

# 5. Behavioral Preservation

Before changing the public boundary, the existing implementation should be characterized sufficiently to distinguish API-refactoring regressions from intentional semantic corrections.

The purpose is not to freeze every existing behavior forever.

The purpose is to establish an observable baseline.

Conceptually:

~~~text
Current Rigel
     │
     ▼
Capture behavioral baseline
     │
     ├── MMIO traces
     ├── timing/deadline traces
     ├── IPL traces
     ├── DMA effects
     ├── selected video state
     └── selected audio state
     │
     ▼
Refactor API
     │
     ▼
Replay equivalent scenarios
     │
     ▼
Compare behavior
~~~

Differences discovered after refactoring should be classified as either:

~~~text
intentional semantic correction
~~~

or:

~~~text
regression caused by API refactoring
~~~

This makes the guiding principle:

> Preserve working Rigel semantics.

objectively testable.

---

# 6. Primary Refactoring Target: The Public Boundary

The largest architectural change should occur at the public API boundary rather than inside the chipset implementation.

The current Rigel API exposes considerably more functionality than Bellatrix requires to host Rigel.

This is useful for:

* debugging;
* testing;
* introspection;
* development tools;
* bus analysis;
* snapshots;
* harness operation.

It should not automatically become the Bellatrix integration contract.

The public surface should therefore be conceptually separated into:

~~~text
Rigel API
   │
   ├── Host Integration API
   │
   │      lifecycle
   │      MMIO
   │      progress
   │      deadlines
   │      IPL
   │      guest memory
   │      input
   │      output
   │
   └── Advanced / Inspection API
          bus inspection
          beam inspection
          snapshots
          diagnostics
          testing controls
          internal-state observation
~~~

Bellatrix should depend only on the Host Integration API unless a future architectural requirement explicitly expands that contract.

---

# 7. Opaque Rigel Instance

The primary Rigel object should remain opaque to hosts.

Conceptually:

~~~c
struct rigel;
~~~

Bellatrix should never require direct access to internal chipset structures.

The lifecycle should conceptually remain:

~~~c
struct rigel *rigel_create(...);

void
rigel_reset(...);

void
rigel_destroy(...);
~~~

Internal structures representing:

* Agnus;
* Denise;
* Paula;
* CIA;
* Copper;
* Blitter;
* beam state;
* DMA scheduling;
* interrupt state;

must remain inaccessible through the normal Bellatrix integration boundary.

---

# 8. Separate Hardware Configuration from Host Services

One of the most useful API cleanups is to distinguish two fundamentally different concepts:

~~~text
What hardware Rigel should model

versus

What services the host provides
~~~

These should eventually be represented separately.

Conceptually:

~~~c
struct rigel_config {
    enum rigel_chipset chipset;
    enum rigel_video_standard video_standard;
    uint32_t chip_ram_size;
    ...
};

struct rigel_host_ops {
    uint8_t  (*mem_read8)(void *ctx, uint32_t guest_addr);
    uint16_t (*mem_read16)(void *ctx, uint32_t guest_addr);
    uint32_t (*mem_read32)(void *ctx, uint32_t guest_addr);

    void (*mem_write8)(void *ctx, uint32_t guest_addr, uint8_t value);
    void (*mem_write16)(void *ctx, uint32_t guest_addr, uint16_t value);
    void (*mem_write32)(void *ctx, uint32_t guest_addr, uint32_t value);

    void (*log)(void *ctx, int level, const char *message);
};
~~~

Creation could then conceptually become:

~~~c
struct rigel *
rigel_create(
    const struct rigel_config *config,
    const struct rigel_host_ops *host_ops,
    void *host_context);
~~~

The exact API may differ.

The important architectural separation is:

~~~text
rigel_config
      │
      └── describes classic hardware

rigel_host_ops
      │
      └── describes services supplied by the host

host_context
      │
      └── identifies host-specific state
~~~

This prevents configuration from becoming a generic container for host integration mechanisms.

---

# 9. Chip RAM Configuration

Fields such as:

~~~c
uint32_t chip_ram_size;
~~~

must describe the chipset-visible memory topology.

They must not imply that Rigel owns the allocation.

The relationship should remain:

~~~text
Bellatrix
    │
    ├── allocates guest memory
    ├── maps guest memory
    └── provides backing
            │
            ▼
      guest physical memory
            │
            ▼
          Rigel
            │
            └── interprets chipset-visible
                Chip RAM semantics
~~~

Rigel owns:

* Agnus-visible address rules;
* chipset address masking;
* chipset pointer interpretation;
* DMA accessibility;
* classic Chip RAM semantics.

Bellatrix owns:

* allocation;
* mapping;
* backing storage;
* host representation.

---

# 10. Clarify Memory Callback Semantics

Memory callback addresses should have an explicit architectural meaning.

The recommended contract is:

> Addresses passed to host memory callbacks are guest physical addresses.

Therefore:

~~~c
mem_read16(ctx, guest_physical_address);
~~~

must not ambiguously mean:

* Chip RAM array offset;
* chipset-generated address;
* M68K MMIO address;
* host pointer.

The complete translation becomes:

~~~text
chipset register
      │
      ▼
chipset-generated address
      │
      ▼
Rigel address masking / decoding
      │
      ▼
guest physical address
      │
      ▼
host memory callback
      │
      ▼
Bellatrix memory backend
~~~

This distinction is particularly important once Bellatrix provides multiple guest-memory regions.

---

# 11. Define Memory Failure Semantics Before API v1

Before the host memory interface becomes part of API Version 1, the implementation must determine whether memory callbacks are infallible by contract or require explicit failure semantics.

A simple interface such as:

~~~c
uint16_t
mem_read16(
    void *ctx,
    uint32_t guest_physical_address);
~~~

implicitly assumes that every address passed to the host memory backend can produce a valid result.

That may be correct if Rigel guarantees that only valid, configured guest physical addresses can reach the callback.

If this guarantee cannot be made, the API may require explicit failure semantics.

Conceptually:

~~~c
rigel_status_t
mem_read16(
    void *ctx,
    uint32_t guest_physical_address,
    uint16_t *value);
~~~

This document does not mandate either form.

The API convergence process must determine and document:

* whether memory callbacks can fail;
* which component validates addresses;
* what happens for inaccessible memory;
* what happens for addresses outside configured Chip RAM;
* whether invalid DMA accesses produce open-bus behavior, ignored writes, hardware-specific behavior, or integration errors.

Classic hardware behavior must not accidentally become a host API failure.

The distinction between:

~~~text
valid classic hardware behavior
~~~

and:

~~~text
host integration failure
~~~

must remain explicit.

---

# 12. Preserve Callback-Based Memory Initially

The callback memory model is sufficient for the initial Bellatrix integration.

It should not be replaced prematurely with direct mappings.

Initial model:

~~~text
Rigel
  │
  ▼
host memory callback
  │
  ▼
Bellatrix guest memory
~~~

This has several advantages during convergence:

* explicit ownership;
* simple instrumentation;
* easy harness implementation;
* controlled JIT invalidation;
* clear memory semantics;
* minimal host coupling.

Optimization should occur only after correctness is demonstrated.

---

# 13. Direct Memory Windows as a Later Optimization

A future optimization may expose validated direct memory windows.

Conceptually:

~~~c
struct rigel_memory_window {
    uint32_t guest_base;
    size_t size;
    void *host_ptr;
};
~~~

The optimized relationship could become:

~~~text
Rigel
  │
  ├── callback memory
  │
  └── validated direct window
             │
             ▼
        guest memory
~~~

However, direct access must remain an optimization of the same memory contract.

It must not redefine memory ownership.

Rigel must not become aware of:

* Bellatrix allocators;
* ARM page tables;
* Emu68 translation metadata;
* Raspberry Pi memory layout.

---

# 14. Canonical MMIO Boundary

The current Rigel API already contains subsystem-specific MMIO operations.

For example, custom-register operations naturally use register offsets.

That internal model should remain useful.

However, Bellatrix should use a canonical M68K-visible MMIO transaction boundary.

Conceptually:

~~~text
Bellatrix / execution engine
            │
            ▼
M68K-visible MMIO transaction
            │
            ├── address
            ├── width
            ├── read/write direction
            └── value where applicable
            │
            ▼
           Rigel
~~~

For example, Bellatrix should be able to present the logical transaction:

~~~text
WRITE

M68K address = 0x00DFF096
width        = 16 bits
value        = 0x8200
~~~

without needing to understand that:

~~~text
0x00DFF096
      │
      ▼
custom register region
      │
      ▼
offset 0x096
~~~

That interpretation should belong to Rigel.

The exact C representation of the canonical MMIO boundary is intentionally not frozen by this document.

It may eventually use width-specific entry points, a generic transaction interface, or another representation that preserves the required semantics.

---

# 15. Canonical MMIO Does Not Make Rigel a Global Dispatcher

The canonical MMIO API does not imply that Rigel receives every M68K address.

Bellatrix/Emu68 remains responsible for determining which registered provider owns an address-space region.

Conceptually:

~~~text
M68K address
     │
     ▼
Emu68 / Bellatrix address dispatcher
     │
     ├── native provider
     │
     ├── Rigel provider
     │
     └── unmapped
             │
             ▼
       normal behavior
~~~

Only an access assigned to a Rigel provider reaches the Rigel MMIO interface.

Therefore Bellatrix may know:

~~~text
0xDFFxxx
belongs to a Rigel compatibility region
~~~

without knowing:

~~~text
0xDFF096
is DMACON
~~~

The latter is classic chipset knowledge and belongs to Rigel.

The responsibility boundary is:

~~~text
Bellatrix / Emu68
        │
        │ determine provider
        ▼
      Rigel
        │
        │ interpret address inside
        │ compatibility domain
        ▼
chipset component
~~~

Rigel MUST NOT become a generic fallback handler for the complete M68K address space.

---

# 16. Preserve Internal Region-Specific MMIO

Canonical external MMIO does not require removing useful internal APIs.

The implementation may remain conceptually:

~~~text
canonical MMIO transaction
          │
          ▼
    Rigel MMIO router
          │
          ├── custom
          │      │
          │      ▼
          │  custom register handler
          │
          ├── CIAA
          │
          └── CIAB
~~~

This is preferable to moving address decoding into Bellatrix.

Bellatrix should determine that an address belongs to a Rigel-registered region.

Rigel should determine what that address means inside the compatibility hardware environment.

---

# 17. MMIO Values Must Remain M68K-Logical

The canonical MMIO interface should operate on M68K-visible logical values.

For example, the logical transaction:

~~~text
WRITE

address = 0x00DFF096
width   = 16 bits
value   = 0x8200
~~~

must mean:

~~~text
M68K-visible value = 0x8200
~~~

regardless of ARM host byte order.

Endianness conversion must occur at a clearly defined boundary.

ARM-native representation must not leak into Rigel register semantics.

---

# 18. Define MMIO Width and Alignment Semantics

Before the canonical MMIO interface becomes part of API Version 1, the supported transaction widths and alignment behavior must be explicitly defined.

Conceptual transactions may include:

~~~text
8-bit read
16-bit read
32-bit read

8-bit write
16-bit write
32-bit write
~~~

The existence of these conceptual transactions must not imply that all classic hardware regions naturally support all widths.

The API convergence process must determine and document:

* which MMIO widths are supported;
* whether support differs between compatibility regions;
* required address alignment for each width;
* behavior of misaligned accesses;
* behavior of historically unusual access widths;
* whether unsupported widths are rejected or represented through defined classic bus behavior;
* whether a wider access represents one logical transaction or multiple ordered classic bus transactions.

The following concepts must remain distinct:

~~~text
CPU transaction width
        │
        ▼
classic bus semantics
        │
        ▼
register implementation width
~~~

An implementation must not assume without an explicit semantic rule that:

~~~text
32-bit read at A
~~~

is necessarily equivalent to:

~~~text
16-bit read at A
followed by
16-bit read at A + 2
~~~

If a 32-bit operation is represented internally as multiple classic bus accesses, their:

* ordering;
* side effects;
* timing visibility;
* fault behavior;

must be explicitly defined.

The same principle applies to byte accesses on registers whose historical hardware semantics are naturally word-oriented.

MMIO width and alignment behavior must therefore be part of the public transaction contract rather than an accidental consequence of the ARM host implementation.

---

# 19. MMIO Transactions Are Observable Hardware Operations

Canonical MMIO operations must be treated as hardware transactions rather than ordinary memory accesses.

A read transaction may have hardware-visible consequences.

Similarly, a write represents an observable transaction whose:

* address;
* width;
* value;
* ordering;
* number of occurrences;

may be semantically significant.

Therefore Bellatrix, Emu68, or another execution engine must not assume that Rigel MMIO transactions may be freely:

* cached;
* eliminated;
* duplicated;
* combined;
* split;
* reordered;

unless the Rigel transaction contract explicitly permits that transformation.

Conceptually:

~~~text
M68K execution
      │
      ▼
observable MMIO transaction
      │
      ├── address
      ├── width
      ├── value
      ├── ordering
      └── occurrence
      │
      ▼
Rigel
      │
      ▼
classic hardware semantics
~~~

For example:

~~~text
read A
read A
~~~

must not automatically become:

~~~text
read A once
reuse result
~~~

because two hardware reads may not be semantically equivalent to one read.

Likewise:

~~~text
write A
write B
~~~

must not be reordered merely because the host architecture or JIT would normally permit such optimization for ordinary memory.

If Rigel internally decomposes a transaction into multiple classic bus operations, that decomposition belongs to Rigel's defined MMIO semantics.

The host must preserve the M68K-visible transaction presented at the public boundary.

This rule is especially important for translated execution engines:

> MMIO optimization must preserve Rigel-defined observable transaction semantics.

The canonical MMIO API is therefore a hardware transaction boundary, not merely a convenience memory-access API.

---

# 20. Define MMIO Transaction Result and Failure Semantics

Before the canonical MMIO interface becomes part of API Version 1, the convergence process must determine how transaction completion and exceptional conditions are represented at the host boundary.

Possible conditions include:

* unsupported access width;
* misaligned access;
* access to an address hole inside a Rigel-owned compatibility region;
* classic hardware behavior that does not correspond to an ordinary successful memory access;
* an internal integration failure unrelated to guest-visible hardware behavior.

The first question should be whether every transaction assigned to Rigel can be resolved entirely through classic hardware semantics.

Conceptually:

~~~text
M68K MMIO transaction
        │
        ▼
      Rigel
        │
        ▼
classic hardware behavior
        │
        ▼
transaction completed
~~~

If so, no generic host-visible failure mechanism may be required.

Alternatively, if the public boundary must represent transaction status explicitly, the API may require a conceptual result such as:

~~~text
MMIO transaction result
        │
        ├── completed
        │
        ├── defined guest-visible hardware condition
        │
        └── host integration failure
~~~

This document does not mandate a particular C representation.

The MMIO convergence phase must determine and document:

* whether every Rigel-owned MMIO transaction resolves through classic hardware semantics;
* whether the host needs an explicit transaction-result representation;
* how unsupported widths are represented;
* how misaligned accesses are represented;
* how compatibility-region address holes are represented;
* whether any guest-visible bus or fault condition must cross the API boundary;
* how genuine host integration failures are reported.

The distinction between:

~~~text
guest-visible classic hardware behavior
~~~

and:

~~~text
host integration failure
~~~

must remain explicit.

A valid or historically defined hardware response must not accidentally become an API error merely because it is unusual from the host's perspective.

Likewise, a genuine integration failure must not silently become invented classic hardware behavior.

---

# 21. Keep Address Namespaces Explicit

The convergence should preserve explicit terminology for different address spaces.

At minimum:

~~~text
M68K MMIO address
        │
        │ CPU-visible hardware address
        ▼
Rigel MMIO routing


Chipset-generated address
        │
        │ interpreted according to
        │ classic chipset rules
        ▼
Rigel address masking / decoding
        │
        ▼
Guest physical address
        │
        │ supplied to host memory backend
        ▼
Host representation / pointer
~~~

These concepts must not be collapsed merely because a particular implementation can map between them cheaply.

Recommended terminology in the public API should therefore distinguish names such as:

~~~text
m68k_address

chipset_generated_address

guest_physical_address

host_ptr
~~~

where appropriate.

In particular, a classic MMIO address such as:

~~~text
0x00DFF096
~~~

must not be described as a `guest_physical_address` in the public contract.

---

# 22. Preserve the Existing Temporal Model

The existing Rigel temporal model already provides the essential concepts required by Bellatrix.

The convergence should preserve the conceptual capabilities:

~~~text
current Rigel time

next Rigel deadline

advance Rigel according to reported execution progress
~~~

These map naturally onto the integration model:

~~~text
Emu68
  │
  ▼
execution progress
  │
  ▼
Bellatrix
  │
  ▼
Rigel
  │
  ├── advances authoritative chipset timeline
  └── returns next synchronization deadline
~~~

The existing timing implementation should therefore be refined and documented rather than replaced.

The exact API representation of advancement remains intentionally undecided at this stage.

In particular:

~~~text
semantic capability
        │
        ▼
advance Rigel according
to execution progress
        │
        ▼
API design decision
       / \
      /   \
advance   advance_to
(delta)   (absolute)
~~~

The temporal API freeze phase should determine which representation best expresses the existing model without changing its ownership semantics.

---

# 23. Rigel Owns the Authoritative Timeline

The public API and implementation should make one point unambiguous:

> Rigel owns the authoritative chipset timeline.

Bellatrix may maintain:

* accumulated CPU progress;
* synchronization accounting;
* execution budgets;
* scheduling information.

Those values are not authoritative chipset time.

Conceptually:

~~~text
Bellatrix progress accounting
             │
             ▼
     execution progress
             │
             ▼
           Rigel
             │
             ▼
 authoritative chipset timeline
             │
      ┌──────┼─────────┐
      │      │         │
     beam   DMA      Copper
                    Blitter
                    Paula
                    CIA
~~~

No second beam or chipset clock should exist in Bellatrix.

---

# 24. Freeze the Progress Unit Before API v1

Before the temporal API becomes a stable Version 1 API, one canonical progress representation must be selected.

Possible models include:

~~~c
rigel_advance_cycles(...);
~~~

or:

~~~c
rigel_advance_ns(...);
~~~

or an equivalent fixed virtual unit.

The choice should optimize for:

* deterministic conversion;
* sufficient precision;
* efficient Emu68 integration;
* overflow behavior;
* testability;
* portability across hosts.

It must not represent host elapsed time.

The earlier conceptual API descriptions in this document do not commit Version 1 to a delta-based or absolute-time advancement model.

---

# 25. Define Deadline Representation Explicitly

The current temporal model should be made explicit about whether deadlines are:

~~~text
absolute virtual timestamps
~~~

or:

~~~text
relative deltas
~~~

The Version 1 API must select one representation.

For example, an absolute model might conceptually be:

~~~c
rigel_time_t now =
    rigel_get_time(rigel);

rigel_time_t deadline =
    rigel_get_next_deadline(rigel);
~~~

where both values belong to the same virtual timeline.

Alternatively, a relative API could explicitly express:

~~~c
rigel_duration_t delta =
    rigel_time_until_deadline(rigel);
~~~

What should be avoided is an API where callers must infer the semantics.

---

# 26. Evaluate `advance()` Versus `advance_to()`

Before the temporal API is frozen, the convergence work should explicitly evaluate how the semantic capability:

~~~text
advance Rigel according to execution progress
~~~

should be represented at the public API boundary.

A delta-oriented API could conceptually use:

~~~c
rigel_advance(
    rigel,
    elapsed);
~~~

An absolute-time API could conceptually use:

~~~c
rigel_advance_to(
    rigel,
    reached_time);
~~~

An absolute model has the potential advantage that:

~~~text
current time
deadline
advance target
~~~

all inhabit the same explicit virtual timeline.

A delta model may integrate more naturally with execution engines that report accumulated progress.

This document does not select either model.

The decision belongs to the temporal API freeze phase.

Until then, references such as:

~~~text
advance Rigel

obtain next deadline
~~~

should be understood as conceptual capabilities rather than final ABI signatures.

---

# 27. Preserve Overshoot Support

Rigel should continue to tolerate execution progress crossing a deadline.

Conceptually:

~~~text
deadline = 100

CPU reaches = 112

Rigel:
    process event at 100
    continue through remaining 12
~~~

This is important because a translated M68K execution engine may not always stop exactly at a chipset boundary.

However:

> Overshoot tolerance is a correctness property, not a scheduling strategy.

Bellatrix should still attempt to respect Rigel deadlines reasonably closely.

---

# 28. Preserve Returned Host-Visible Results

Rigel already has a useful model where stepping can return information about externally relevant state changes.

This is preferable to introducing a generic asynchronous callback from Rigel into Bellatrix.

Conceptually:

~~~text
Bellatrix
    │
    │ Rigel advance
    ▼
Rigel
    │
    ▼
step result
    │
    ├── CPU-visible state changed
    ├── output became available
    └── other explicitly host-visible result
~~~

This direction has important properties:

* deterministic ordering;
* no arbitrary reentrancy;
* simple harness reproduction;
* host remains in control of execution;
* Rigel does not acquire Bellatrix dependencies.

Internal hardware events should remain internal unless they cross the host boundary for a defined reason.

For example:

~~~text
Blitter completes
       │
       ├── changes internal state
       └── may affect INTREQ
~~~

does not necessarily imply that Bellatrix should receive a dedicated:

~~~text
BLIT_DONE
~~~

host event.

The host should observe the externally relevant consequence through the appropriate boundary.

---

# 29. Avoid Generic Host Event Callbacks

A generic callback such as:

~~~c
signal_event(ctx, event);
~~~

should not be introduced merely for convenience.

It risks becoming:

~~~text
Rigel
  │
  └── arbitrary escape into Bellatrix
~~~

Instead, prefer:

~~~text
Rigel call
   │
   ▼
explicit return value
   │
   ▼
Bellatrix reacts
~~~

or narrow interfaces with well-defined semantics when asynchronous communication is genuinely required.

---

# 30. Preserve Rigel IPL as the Interrupt Boundary

The normal Bellatrix integration should require only the compatibility-domain IPL.

Conceptually:

~~~c
unsigned
rigel_get_ipl(const struct rigel *rigel);
~~~

Bellatrix then performs:

~~~text
native_ipl ─────┐
                │
                ▼
          IPL arbitration
                │
                ▼
              M68K
                ▲
                │
rigel_ipl ──────┘
~~~

Bellatrix should not need to inspect Rigel's individual interrupt sources to perform normal CPU interrupt delivery.

---

# 31. Keep INTREQ and INTENA for Inspection if Useful

Existing APIs exposing:

~~~text
INTREQ
INTENA
~~~

may remain useful for:

* debugging;
* harness inspection;
* diagnostics;
* tests;
* state visualization.

They should not become required inputs to Bellatrix interrupt arbitration.

The distinction should be:

~~~text
Host integration:
    rigel_get_ipl()

Inspection:
    rigel_get_intreq()
    rigel_get_intena()
~~~

Bellatrix should not reconstruct Rigel IPL from `INTREQ` and `INTENA`.

That calculation belongs to Rigel.

---

# 32. Preserve Bus Observation as an Advanced Capability

Rigel's bus-observation facilities should not be removed merely because they are not required by the minimal Bellatrix integration.

Capabilities conceptually equivalent to:

~~~text
get bus state

get next bus change

determine whether CPU can access Chip RAM

determine CPU resume time
~~~

may become important for tighter CPU/chipset synchronization.

In particular, they may later support:

~~~text
M68K CPU
   │
   ▼
Chip RAM access
   │
   ▼
Rigel bus ownership
   │
   ├── CPU may access
   │
   └── CPU must wait
             │
             ▼
        resume time
~~~

This is distinct from the basic deadline interface.

---

# 33. Do Not Make Bus Contention Mandatory in Phase 1

The initial Bellatrix adapter should not require detailed bus observation unless needed for correctness of the first integration.

Recommended progression:

~~~text
Initial integration

MMIO
progress
deadline
IPL
DMA
memory coherence

        │
        ▼

Advanced integration

fine-grained Chip RAM contention
bus ownership
CPU stalls
bus-change observation
~~~

The existing Rigel bus infrastructure should be preserved so advanced integration does not require redesigning chipset internals.

This preserves the possibility of a future relationship such as:

~~~text
Emu68 executes M68K
        │
        ├── Fast RAM access
        │      └── no Rigel involvement
        │
        └── Chip RAM access
               │
               ▼
          Rigel bus model
               │
             busy?
             /   \
           no     yes
           │       │
         access   stall until
                  resume time
~~~

Rigel remains authoritative for classic bus semantics.

Bellatrix does not reproduce Agnus bus-slot logic.

---

# 34. Reconsider `rigel_chipset_wire()`

Any public function that exposes internal chipset wiring should be reviewed carefully.

If an operation such as:

~~~c
rigel_chipset_wire(...);
~~~

exists primarily to connect internal Rigel components, it should probably not form part of the normal host integration API.

The desired lifecycle is:

~~~text
rigel_create()
      │
      ├── allocate internal state
      ├── configure hardware
      ├── wire internal components
      └── return complete opaque instance
~~~

The host should receive a valid Rigel instance rather than constructing internal chipset relationships itself.

If runtime reconfiguration requires rewiring, that requirement should be represented by a specific public operation rather than exposing generic internal wiring.

---

# 35. Define Lifecycle and Reset Semantics Before API v1

Before the lifecycle API becomes part of Version 1, creation, reset, and destruction semantics must be explicitly defined.

At minimum, the public contract should distinguish conceptually between:

~~~text
create
  │
  ▼
defined initial instance state


cold reset
  │
  ▼
defined classic power-on/reset state


warm reset
  │
  ▼
defined classic machine reset semantics


destroy
  │
  ▼
instance becomes unusable
~~~

The API convergence process must determine and document:

* what state exists immediately after `rigel_create()`;
* whether `rigel_create()` implicitly performs a cold reset;
* whether an explicit initial reset is required before MMIO or advancement;
* what internal state is reset by a cold reset;
* what internal state is reset or preserved by a warm reset;
* what externally supplied state survives each reset;
* whether guest memory is modified by reset;
* whether host callbacks remain installed across reset;
* whether configuration remains fixed across reset;
* whether output queues or pending host-visible results are discarded or preserved;
* whether the Rigel virtual timeline is restarted, preserved, or otherwise transformed;
* what operations are valid during each lifecycle state.

Conceptually:

~~~c
rigel_reset(
    rigel,
    RIGEL_RESET_COLD);

rigel_reset(
    rigel,
    RIGEL_RESET_WARM);
~~~

The existence of reset enum values must not precede a clear definition of their semantics.

In particular:

> Bellatrix must request a reset class. Rigel must own the classic hardware semantics of that reset class.

Bellatrix must not reproduce rules such as:

~~~text
reset Copper this way

preserve CIA state that way

clear this interrupt register

restart this beam state
~~~

Those rules belong to Rigel.

Likewise, resetting Rigel must not implicitly reset unrelated Bellatrix or Raspberry Pi hardware.

Lifecycle semantics should therefore be frozen together with the Version 1 host contract rather than treated as a trivial implementation detail.

---

# 36. Reconsider Cycle-Exact Control as Integration API

Development controls conceptually equivalent to:

~~~c
rigel_set_cycle_exact(...);
rigel_get_cycle_exact(...);
~~~

may remain valuable.

However, Bellatrix should not normally need to know which internal mechanism Rigel uses to satisfy its timing contract.

These controls are better classified as:

~~~text
advanced configuration

testing

diagnostics

A/B validation
~~~

rather than mandatory host integration operations.

The distinction should be reflected in API organization.

---

# 37. Snapshots Should Remain Outside the Minimal Host Contract

Snapshot functionality is valuable and should not be removed.

However:

~~~text
snapshot creation
snapshot restoration
state inspection
~~~

are not necessary to define the basic Bellatrix/Rigel integration.

They should remain available through an advanced or tooling API.

The minimal Bellatrix adapter should not depend on snapshot functionality.

---

# 38. Video Output

Rigel should continue to own classic video-generation semantics.

Bellatrix should own presentation.

Conceptually:

~~~text
Denise / chipset
       │
       ▼
     Rigel
       │
       ▼
host-independent video output
       │
       ▼
Bellatrix video adapter
       │
       ▼
VC4 / framebuffer / future output
~~~

No VC4 knowledge should enter Rigel.

The precise output representation does not need to be frozen as part of the first API cleanup unless required immediately.

Possible future representations include:

* completed frames;
* scanlines;
* incremental raster data;
* another host-independent surface.

---

# 39. Audio Output

The equivalent boundary should exist for Paula audio.

~~~text
Paula
  │
  ▼
Rigel
  │
  ▼
host-independent audio output
  │
  ▼
Bellatrix audio adapter
  │
  ▼
native audio hardware
~~~

Rigel must remain unaware of:

* HDMI audio;
* PWM;
* USB audio;
* Raspberry Pi-specific audio transport.

Existing audio generation should be preserved while presentation remains host-owned.

---

# 40. Input

Input should follow the reverse direction.

~~~text
USB / Bluetooth / native input
              │
              ▼
          Bellatrix
              │
              ▼
       input adaptation
              │
              ▼
            Rigel
              │
              ▼
      classic hardware state
~~~

Rigel should receive classic hardware-facing input information.

It should not receive:

* USB descriptors;
* HID reports;
* Bluetooth objects;
* Raspberry Pi controller state.

Different classic hardware interfaces may have different APIs.

There is no requirement to invent one generic input structure for all devices.

---

# 41. Reentrancy

The Version 1 API should explicitly define Rigel instances as non-reentrant unless otherwise documented.

For example:

~~~text
Rigel advance
      │
      ▼
host mem_read()
      │
      ╳
      └── must not re-enter
          the same Rigel instance
~~~

This allows the initial implementation to remain simple and deterministic.

It avoids introducing synchronization machinery into Rigel merely to accommodate hypothetical host behavior.

---

# 42. Thread Affinity

A Rigel instance may initially be treated as single-thread-affine.

The host should serialize calls into the instance.

The API must not encode a specific ARM core.

Therefore Rigel must remain unaware of:

~~~text
Core 0
Core 1
Core 2
Core 3

WFE
SEV

Bellatrix queues

Bellatrix scheduler
~~~

This allows Bellatrix to change core topology without changing the Rigel API.

---

# 43. API Version Versus Binary ABI Stability

Rigel API Version 1 should define the stable public source-level contract.

Binary ABI stability should not be implied unless explicitly specified.

This distinction is important because structures such as:

~~~c
struct rigel_config;
struct rigel_host_ops;
rigel_time_t;
enum rigel_*;
~~~

may otherwise acquire permanent binary-layout compatibility requirements.

For the initial Version 1 contract, the recommended rule is:

> Rigel API Version 1 defines a stable public source-level interface. Binary ABI stability is not guaranteed unless separately documented.

This allows Bellatrix, the harness, and other hosts to rebuild against compatible public headers without requiring indefinite preservation of structure layouts.

If binary ABI stability becomes a future requirement, the interface should explicitly introduce the necessary mechanisms.

These may include concepts such as:

~~~c
struct rigel_config {
    uint32_t struct_size;
    uint32_t api_version;
    ...
};
~~~

Such mechanisms should not be introduced merely in anticipation of a requirement that does not yet exist.

---

# 44. Recommended Public API Organization

The final header organization does not need to follow one particular filesystem layout, but the conceptual separation should be clear.

One possible organization is:

~~~text
include/rigel/
│
├── rigel.h
│
│   Minimal host integration surface
│
├── rigel_types.h
│
├── rigel_config.h
│
├── rigel_host.h
│
├── rigel_mmio.h
│
├── rigel_time.h
│
├── rigel_irq.h
│
├── rigel_video.h
│
├── rigel_audio.h
│
├── rigel_input.h
│
└── advanced/
    │
    ├── rigel_bus.h
    ├── rigel_snapshot.h
    ├── rigel_debug.h
    └── rigel_inspect.h
~~~

This exact layout is not normative.

The important property is that the normal host API does not accidentally expose every internal or diagnostic capability.

---

# 45. `rigel.h` Should Become Deliberately Small

The umbrella header used by Bellatrix should expose only what is required to host a Rigel instance.

Conceptually:

~~~c
/* lifecycle */

struct rigel *
rigel_create(...);

void
rigel_destroy(...);

void
rigel_reset(...);


/* interrupt */

unsigned
rigel_get_ipl(...);
~~~

The remaining core capabilities should be represented without prematurely freezing their exact C signatures.

Conceptually:

~~~text
MMIO

    submit M68K-visible read transaction

    submit M68K-visible write transaction

    obtain read value where applicable

    preserve:
        address
        width
        ordering
        side effects
        occurrence

    represent transaction result/failure
    according to the semantics frozen
    during MMIO convergence


Timing

    obtain current Rigel time

    obtain next synchronization deadline

    advance Rigel according to
    reported execution progress
~~~

The exact functions should be derived from the integration specification and convergence phases rather than copied mechanically from this document.

In particular:

* MMIO entry points are not frozen until width, alignment, decomposition, ordering, side-effect, and result semantics are defined;
* the temporal signatures are not frozen until the progress and deadline model is defined;
* reset classes are not frozen until lifecycle and reset semantics are defined.

The Version 1 header should be the consequence of the resolved contract.

It should not be the place where unresolved architectural questions are silently answered through convenient C signatures.

---

# 46. Internal APIs Should Remain Internal

The refactoring should identify functions that are currently public only because historical implementation convenience made them public.

Candidates should be classified as:

~~~text
Host API

Advanced API

Inspection API

Test API

Internal API
~~~

Anything classified as internal should move out of the stable public API.

This is particularly important before declaring `RIGEL_API_VERSION 1`.

Once Version 1 is established, unnecessary public symbols become source-compatibility obligations.

---

# 47. Bellatrix Adapter Should Remain Small

The resulting Bellatrix-side code should conceptually look like:

~~~text
Bellatrix
   │
   └── rigel_adapter
          │
          ├── create/configure Rigel
          ├── register MMIO regions
          ├── forward Rigel-owned Emu68 MMIO transactions
          ├── provide guest-memory operations
          ├── report execution progress
          ├── observe deadlines
          ├── obtain rigel_ipl
          ├── participate in IPL arbitration
          ├── adapt native input
          └── present video/audio
~~~

The MMIO relationship should remain:

~~~text
Emu68
  │
  ▼
Bellatrix address dispatcher
  │
  │ selects Rigel provider
  ▼
Bellatrix Rigel adapter
  │
  │ forwards M68K-visible transaction
  ▼
Rigel
  │
  │ interprets compatibility-region address
  ▼
classic hardware semantics
~~~

The adapter must preserve the observable MMIO transaction presented by the execution engine unless Rigel's public contract explicitly permits a transformation.

The adapter must not contain:

~~~text
Copper logic

Blitter logic

Denise logic

Paula logic

CIA timing

beam calculations

INTREQ semantics

INTENA semantics

Agnus DMA addressing
~~~

If such code begins accumulating in `rigel_adapter`, the boundary has failed.

---

# 48. Recommended Migration Strategy

The migration should be incremental.

A large rewrite would introduce unnecessary risk because the current Rigel implementation already contains working hardware behavior.

Recommended strategy:

~~~text
Existing Rigel
      │
      ▼
capture behavioral baseline
      │
      ▼
classify current public API
      │
      ▼
define minimal host-facing API
      │
      ▼
introduce compatibility wrappers if needed
      │
      ▼
move harness to candidate API
      │
      ▼
move Bellatrix to candidate API
      │
      ▼
integration validation
      │
      ▼
API review
      │
      ▼
classify remaining APIs
      │
      ├── advanced
      ├── debug
      ├── test
      └── internal
      │
      ▼
remove obsolete public exposure
      │
      ▼
freeze API Version 1
~~~

At every stage, the chipset behavior should remain testable.

---

# 49. Phase 0 — Behavioral Baseline

Before changing the public API, establish a behavioral characterization baseline for the existing Rigel implementation.

The baseline should capture representative scenarios for:

* MMIO;
* MMIO transaction ordering and side effects;
* timing;
* deadlines;
* interrupt transitions;
* DMA reads;
* DMA writes;
* reset behavior;
* selected video behavior;
* selected audio behavior;
* bus behavior where already tested.

Conceptually:

~~~text
Existing Rigel
      │
      ▼
Known deterministic scenarios
      │
      ▼
Capture observable behavior
      │
      ├── MMIO transaction traces
      ├── timing traces
      ├── memory results
      ├── IPL transitions
      ├── deadlines
      └── output hashes/state
      │
      ▼
Behavioral baseline
~~~

The baseline does not declare every existing behavior correct.

Known bugs may remain explicitly documented as known deviations.

The purpose is to distinguish intentional corrections from accidental regressions introduced during API convergence.

---

# 50. Phase 1 — API Inventory

Before changing exported visibility, classify every currently exported Rigel symbol.

Each symbol should be assigned one category:

~~~text
CORE HOST

ADVANCED HOST

INSPECTION

TEST

INTERNAL

DEPRECATED
~~~

For example:

~~~text
rigel_create
    → CORE HOST

rigel_destroy
    → CORE HOST

rigel_get_ipl
    → CORE HOST

bus inspection
    → ADVANCED / INSPECTION

snapshot
    → ADVANCED

chipset wiring
    → likely INTERNAL

cycle-exact development control
    → ADVANCED / TEST
~~~

No symbol should remain public merely because it is currently public.

---

# 51. Phase 2 — Separate Configuration and Host Operations

Refactor creation so classic hardware configuration and host services become conceptually independent.

Target:

~~~text
rigel_config
      │
      └── hardware model

rigel_host_ops
      │
      └── host services

host_context
      │
      └── host instance
~~~

Update the harness early in this process where practical.

This provides immediate proof that the interface remains independent from Bellatrix.

---

# 52. Phase 3 — Formalize Memory Semantics

Define explicitly:

~~~text
chipset-generated address
        ↓
Rigel address masking / decoding
        ↓
guest physical address
        ↓
host memory API
~~~

Ensure callbacks operate according to this definition.

Determine whether host memory callbacks are:

~~~text
infallible by contract
~~~

or:

~~~text
capable of explicit failure
~~~

and document the result before Version 1.

Add tests covering:

* chipset address masking;
* Chip RAM limits;
* alignment;
* host mapping;
* out-of-range behavior;
* invalid or inaccessible backing where applicable;
* distinction between hardware-visible invalid access and host integration failure.

---

# 53. Phase 4 — Introduce Canonical MMIO

Introduce a canonical M68K-visible MMIO transaction boundary.

Target relationship:

~~~text
Bellatrix / Harness
        │
        ▼
canonical MMIO transaction
        │
        ▼
Rigel MMIO router
        │
        ├── custom
        ├── CIAA
        └── CIAB
~~~

Bellatrix/Emu68 remains responsible for selecting Rigel as the provider for a registered compatibility region.

Rigel remains responsible for interpreting the M68K-visible address within that compatibility domain.

Keep existing subsystem MMIO helpers internally where useful.

During this phase, explicitly define:

* supported MMIO access widths;
* width semantics for each compatibility region;
* alignment requirements;
* misaligned access behavior;
* unsupported-width behavior;
* whether wider operations represent one logical access or multiple ordered classic bus transactions;
* ordering and side-effect semantics for decomposed accesses;
* M68K-visible endianness semantics;
* observability requirements for reads and writes;
* which, if any, transaction transformations are permitted to the host or execution engine;
* whether every Rigel-owned transaction resolves entirely through classic hardware semantics;
* whether an explicit transaction-result representation is required;
* how address holes inside Rigel-owned regions behave;
* whether any guest-visible bus or fault condition must cross the API boundary;
* how genuine host integration failures are represented;
* how guest-visible hardware behavior remains distinct from host integration failure.

The exact C representation of the canonical MMIO interface should be selected only after these semantics are understood.

Possible representations may include:

~~~text
width-specific entry points

or

a generic MMIO transaction interface

or

another explicit representation
~~~

No representation should be selected merely because it is convenient for the current ARM implementation.

Add equivalence tests proving that the new canonical route produces the same chipset behavior as the existing implementation for equivalent transactions.

Tests should also verify that transaction ordering and repeated accesses remain observable where required.

---

# 54. Phase 5 — Freeze Temporal Semantics

Before API v1, decide:

1. canonical execution-progress unit;
2. current-time representation;
3. deadline representation;
4. absolute versus relative deadline semantics;
5. delta-based `advance()` versus absolute `advance_to()` semantics;
6. overflow rules;
7. overshoot semantics.

Then document and test:

~~~text
advance

deadline

overshoot

event ordering

IPL visibility
~~~

This is one of the most important API decisions and should not be left implicit.

---

# 55. Phase 6 — Formalize Lifecycle and Reset Semantics

Before the lifecycle API becomes stable, define the exact relationship between:

~~~text
create

cold reset

warm reset

destroy
~~~

Determine and document:

* post-create state;
* whether creation implies reset;
* valid operations before first reset;
* cold-reset semantics;
* warm-reset semantics;
* timeline behavior across reset;
* interrupt-state behavior across reset;
* DMA-state behavior across reset;
* output-state behavior across reset;
* preservation of host callbacks and host context;
* preservation of configuration;
* interaction with guest memory;
* shutdown ordering requirements.

The host should select the reset class.

Rigel should define the classic hardware semantics of that class.

Add deterministic reset tests to the standalone harness.

---

# 56. Phase 7 — Formalize IPL Boundary

Ensure normal host integration requires only:

~~~text
rigel_ipl
~~~

for classic interrupt delivery.

Preserve detailed interrupt state as inspection functionality where useful.

Test:

~~~text
Rigel interrupt becomes pending
        │
        ▼
rigel_ipl changes
        │
        ▼
Bellatrix observes change
        │
        ▼
M68K IPL arbitration
~~~

and:

~~~text
M68K accepts interrupt
        │
        ╳
        └── Rigel source is NOT
            automatically cleared
~~~

---

# 57. Phase 8 — Candidate API and Harness Migration

Once the fundamental memory, MMIO, timing, lifecycle/reset, and IPL semantics are defined, expose them as a candidate public API.

This stage is intentionally not yet the frozen Version 1 interface.

Conceptually:

~~~text
Candidate Rigel API
        │
        ▼
Standalone Harness
        │
        ▼
Behavioral validation
~~~

The harness should validate:

~~~text
create Rigel

configure Chip RAM

load memory

perform MMIO

verify MMIO ordering/side effects

verify MMIO result semantics

advance time

inspect deadline

observe IPL

inspect DMA effects

cold reset

warm reset

repeat deterministically
~~~

The behavioral baseline from Phase 0 should be replayed through the candidate API where applicable.

---

# 58. Phase 9 — Bellatrix Adapter

Only after the preceding boundaries are sufficiently stable should Bellatrix become a direct consumer of the candidate API.

The adapter should initially implement only:

~~~text
Lifecycle

MMIO

Progress

Deadline

IPL

DMA memory
~~~

The MMIO adapter should route or forward transactions assigned to the Rigel provider.

It should not interpret classic register semantics.

It must preserve transaction identity and ordering according to the canonical Rigel MMIO contract.

It must also preserve the distinction between guest-visible MMIO behavior and any host integration failure represented by the API.

Do not add advanced bus integration, cross-core execution, or zero-copy presentation until this path is validated.

---

# 59. Phase 10 — Integration Validation

The candidate API should then be exercised simultaneously by both real consumers:

~~~text
              Candidate Rigel API
                     │
             ┌───────┴───────┐
             │               │
          Harness         Bellatrix
             │               │
             └───────┬───────┘
                     │
              integration tests
                     │
                     ▼
                API review
~~~

This phase should specifically identify whether real Bellatrix integration exposes missing or incorrectly shaped abstractions.

Examples may include:

* callback granularity;
* memory failure semantics;
* reset distinctions;
* lifecycle ordering;
* deadline semantics;
* MMIO width behavior;
* MMIO alignment behavior;
* MMIO transaction ordering;
* MMIO side-effect preservation;
* MMIO transaction-result semantics;
* MMIO error behavior;
* distinction between guest-visible MMIO behavior and integration failure;
* host-visible result representation.

Such discoveries should be resolved before Version 1 is frozen.

---

# 60. Phase 11 — API Version 1 Review and Freeze

Only after both the standalone harness and Bellatrix operate successfully through the candidate interface should the API be reviewed for Version 1.

The review should ask:

~~~text
Does the harness need anything host-specific?

Does Bellatrix need anything Rigel-internal?

Are any callbacks too generic?

Are any API operations unused?

Are address semantics explicit?

Are MMIO width/alignment semantics explicit?

Are MMIO observability and ordering semantics explicit?

Are MMIO result/failure semantics explicit?

Are guest-visible hardware conditions distinct
from host integration failures?

Are temporal semantics explicit?

Are lifecycle/reset semantics explicit?

Are memory error semantics explicit?

Are ownership rules preserved?
~~~

Only after this review should:

~~~c
#define RIGEL_API_VERSION 1
~~~

represent a stable source-level API commitment.

---

# 61. Phase 12 — Advanced Bus Integration

Once the basic Bellatrix/Rigel integration is correct, evaluate whether Bellatrix should consume Rigel's existing bus-observation facilities.

Potential objective:

~~~text
CPU requests Chip RAM
        │
        ▼
Rigel bus state
        │
       / \
      /   \
available  busy
   │        │
   ▼        ▼
CPU runs   CPU stalls
              │
              ▼
         Rigel resume time
~~~

This may enable more accurate CPU/chipset contention without moving bus semantics into Bellatrix.

This phase should build on the existing Rigel bus model rather than duplicating it.

---

# 62. Phase 13 — Presentation and Input

After the execution, memory, lifecycle, and interrupt boundary is stable, finalize host-independent:

~~~text
video output

audio output

input
~~~

These should remain presentation/adaptation interfaces rather than native-device interfaces.

Rigel must never need to know whether Bellatrix ultimately presents output through:

~~~text
VC4

HDMI

framebuffer

another GPU

another host entirely
~~~

---

# 63. API Compatibility During Migration

Because Rigel is under the same development control as Bellatrix, this is the appropriate moment to make deliberate API-breaking changes if they materially improve the architecture.

Compatibility with an accidental or immature API should not take precedence over establishing a clean Version 1 boundary.

However, changes should still be controlled.

Where useful:

~~~text
old API
   │
   ▼
temporary compatibility wrapper
   │
   ▼
candidate canonical API
~~~

This allows behavior to remain testable while migration proceeds.

Temporary compatibility interfaces should be clearly marked and removed before the Version 1 API is considered frozen.

---

# 64. What Should Not Be Rewritten

The convergence effort should explicitly avoid rewriting working implementation merely for architectural aesthetics.

Do not rewrite without a concrete reason:

* Copper implementation;
* Blitter implementation;
* Denise implementation;
* Paula implementation;
* CIA implementation;
* interrupt priority logic;
* DMA scheduling;
* beam state;
* existing deterministic event scheduling;
* bus ownership model;
* harness infrastructure.

The first question for each change should be:

> Does this implementation violate the new boundary, or is it merely organized differently?

Only actual boundary violations require architectural refactoring.

---

# 65. What Should Change

The following areas are the primary expected changes.

## Required before API v1

1. Capture a behavioral baseline.
2. Audit and classify exported API.
3. Define the minimal host integration surface.
4. Separate hardware configuration from host operations.
5. Make host context explicitly opaque.
6. Define guest-memory callback address semantics.
7. Define memory callback failure semantics.
8. Formalize Chip RAM ownership semantics.
9. Define a canonical M68K-visible MMIO transaction boundary.
10. Preserve provider selection outside Rigel.
11. Define supported MMIO widths.
12. Define MMIO alignment and misalignment semantics.
13. Define semantics for wider accesses and any decomposition into ordered classic bus transactions.
14. Define MMIO observability, ordering, and side-effect semantics.
15. Define which MMIO transaction transformations, if any, are permitted to the host or execution engine.
16. Define MMIO transaction result/failure semantics.
17. Distinguish guest-visible MMIO behavior from host integration failures.
18. Select the concrete C representation of the canonical MMIO boundary only after its semantics are defined.
19. Keep MMIO, chipset DMA, guest physical, and host address spaces distinct.
20. Freeze execution-progress representation.
21. Freeze deadline representation.
22. Select delta-based versus absolute advancement semantics.
23. Preserve Rigel as authoritative chipset timeline.
24. Define lifecycle and reset-state semantics.
25. Define post-create state and whether creation implies reset.
26. Define cold-reset and warm-reset behavior.
27. Formalize IPL as the normal interrupt boundary.
28. Document non-reentrancy.
29. Document initial single-thread affinity.
30. Remove internal construction/wiring details from the normal host API.
31. Establish an explicit public API version.
32. Define whether Version 1 promises source API stability only or binary ABI stability.

## Strongly recommended

33. Separate advanced/debug APIs from the normal host integration API.
34. Keep `INTREQ`/`INTENA` inspection outside normal Bellatrix operation.
35. Keep bus observation as an advanced capability.
36. Keep snapshots outside the minimal integration contract.
37. Preserve returned deterministic host-visible information rather than adding generic asynchronous callbacks.
38. Validate the candidate API with both the harness and Bellatrix before freezing Version 1.

## Later optimizations

39. Direct validated memory windows.
40. Fine-grained CPU/Chip RAM contention.
41. Zero-copy video.
42. Zero-copy audio.
43. Cross-core Rigel execution.
44. Specialized asynchronous notifications where proven necessary.

---

# 66. What Bellatrix Should Not Force Rigel to Become

Bellatrix integration must not cause Rigel to become:

~~~text
a Raspberry Pi library

an Emu68 extension

a VC4 driver

a BCM interrupt adapter

a Bellatrix subsystem

an AROS component
~~~

Rigel remains:

> A host-independent implementation of classic Amiga hardware semantics.

Bellatrix is one host of that implementation.

The standalone harness is another.

Future hosts may exist without requiring architectural changes to Rigel.

---

# 67. Conformance Tests

The refactoring should be considered successful when the following properties are demonstrated.

## Behavioral preservation

Equivalent deterministic scenarios executed before and after API convergence produce equivalent defined hardware behavior unless a difference is explicitly classified as an intentional semantic correction.

## Independent build

~~~text
librigel
~~~

builds without:

~~~text
Bellatrix headers

Emu68 internal headers

Raspberry Pi headers

AROS headers
~~~

## Independent execution

The standalone harness creates and operates a Rigel instance without Bellatrix.

## Canonical MMIO

A canonical M68K-visible MMIO transaction reaches the correct Rigel hardware implementation without Bellatrix interpreting the register.

## MMIO interface neutrality

The Version 1 C representation of MMIO is selected only after the transaction semantics are defined.

The architecture does not require width-specific entry points merely because they are convenient to implement.

## MMIO width semantics

Supported access widths are explicitly defined and produce deterministic classic hardware behavior.

## MMIO alignment semantics

Aligned, misaligned, unsupported, and historically unusual accesses have explicitly defined behavior rather than inheriting accidental ARM host semantics.

## Wider-access semantics

Where a wider MMIO access is represented through multiple classic bus operations, ordering and side effects are explicitly defined and tested.

## MMIO transaction observability

Repeated, ordered, and side-effecting MMIO operations remain distinct observable hardware transactions unless the Rigel contract explicitly defines them as transformable.

## MMIO transaction result semantics

The Version 1 contract explicitly defines how transaction completion, unsupported accesses, misaligned accesses, address holes, and any guest-visible exceptional hardware conditions are represented.

## MMIO failure separation

Guest-visible classic hardware behavior remains semantically distinct from genuine host integration failure.

## Execution-engine independence

The canonical MMIO contract does not depend on Emu68-specific optimization behavior.

An execution engine must preserve Rigel-defined transaction semantics regardless of whether execution is interpreted, translated, or otherwise accelerated.

## Provider isolation

Bellatrix/Emu68 determines whether an address belongs to Rigel.

Rigel interprets the address only after the compatibility provider has been selected.

## Address-space separation

M68K MMIO addresses, chipset-generated addresses, guest physical addresses, and host pointers remain semantically distinct.

## Memory ownership

Rigel performs chipset DMA without allocating Bellatrix guest physical memory.

## Address translation

Chipset-generated addresses are interpreted according to Rigel's classic chipset address rules before the host memory backend receives a guest physical address.

## Memory failure semantics

The Version 1 contract explicitly defines whether host memory operations can fail and how invalid hardware-visible accesses differ from host integration failures.

## Timing ownership

Changing Bellatrix execution accounting does not create a second authoritative chipset clock.

## Determinism

Identical harness inputs produce identical defined outputs.

## Lifecycle correctness

Creation produces a documented instance state and only documented operations are permitted before any required initial reset.

## Cold reset correctness

A cold Rigel reset returns classic hardware state to the documented cold-reset condition without resetting unrelated Bellatrix platform hardware.

## Warm reset correctness

A warm Rigel reset follows the documented classic reset semantics and preserves or resets state according to Rigel-defined rules.

## Interrupt ownership

Bellatrix obtains Rigel IPL without reproducing `INTENA`/`INTREQ` logic.

## JIT independence

Rigel DMA can trigger host-required executable-memory handling without Rigel knowing anything about Emu68 translation internals.

## Adapter isolation

Removing the Bellatrix Rigel adapter removes Rigel integration without requiring structural changes to Bellatrix Core.

## Candidate validation

Both the standalone harness and Bellatrix operate through the candidate host integration API before Version 1 is frozen.

---

# 68. Review Checklist

Every Rigel API refactoring patch should answer:

1. Is this operation actually required by a host?
2. Is it required by Bellatrix specifically or by any generic Rigel host?
3. Is chipset implementation detail leaking through the API?
4. Does the host need to understand a classic hardware register?
5. Is Rigel being given host-specific knowledge?
6. Is hardware configuration being confused with host services?
7. Is an M68K MMIO address being confused with a guest physical address?
8. Is a chipset-generated DMA address being confused with a guest physical address?
9. Is a guest physical address being confused with a host pointer?
10. Is Rigel taking ownership of guest-memory allocation?
11. Is Bellatrix reproducing chipset address-generation, masking, or decoding logic?
12. Is Rigel being turned into a global M68K address dispatcher?
13. Does Bellatrix select only the provider while Rigel owns compatibility-region semantics?
14. Are supported MMIO access widths explicit?
15. Are MMIO alignment and misalignment semantics explicit?
16. If a wider MMIO access is decomposed, are ordering and side effects explicitly defined?
17. Can an MMIO read or write have observable hardware side effects?
18. Could Bellatrix or the execution engine incorrectly cache, eliminate, duplicate, combine, split, or reorder an MMIO transaction?
19. Are permitted MMIO transaction transformations explicitly defined rather than assumed?
20. Are MMIO transaction result semantics explicit?
21. Is guest-visible MMIO behavior being confused with a host integration failure?
22. Has the concrete MMIO C representation been selected only after transaction semantics were defined?
23. Are memory callback failure semantics defined?
24. Is valid classic hardware behavior being incorrectly reported as a host integration error?
25. Is Bellatrix creating authoritative chipset timing state?
26. Is a wall clock entering chipset semantics?
27. Is a deadline's representation explicit?
28. Is advancement explicitly defined as delta-based or absolute?
29. Does overshoot remain deterministic?
30. Are creation and initial-state semantics explicit?
31. Are cold-reset and warm-reset semantics explicit?
32. Is Bellatrix encoding chipset-specific reset behavior that belongs to Rigel?
33. Is Bellatrix reconstructing IPL from internal Rigel state?
34. Is CPU interrupt acceptance being confused with source acknowledgement?
35. Is an internal hardware event being exposed to the host without a defined reason?
36. Is a generic callback being introduced unnecessarily?
37. Can a callback re-enter the same Rigel instance?
38. Is an advanced/debug facility accidentally becoming part of the core host API?
39. Can the standalone harness still exercise the same implementation?
40. Does this change actually require modifying chipset internals?
41. Would another non-Bellatrix host still be able to implement this interface cleanly?
42. Does the change preserve the Phase 0 behavioral baseline, or is any deviation explicitly intentional?
43. Has the candidate interface been exercised by both the harness and Bellatrix before being considered stable?
44. Is binary ABI stability being accidentally promised where only source API stability is intended?

If these questions cannot be answered cleanly, the API change should be reconsidered.

---

# 69. Target Architecture

The target relationship is:

~~~text
                        Bellatrix
                            │
                   Bellatrix Rigel Adapter
                            │
           ┌────────────────┼────────────────┐
           │                │                │
          MMIO           Progress           IPL
           │                │                │
           └────────────────┼────────────────┘
                            │
                     public Rigel API
                            │
                            ▼
                        librigel
                            │
          ┌─────────────────┼─────────────────┐
          │                 │                 │
        Agnus             Denise            Paula
          │                                   │
     Copper/Blitter                        CIAA/CIAB
          │                                   │
          └────────────── DMA ────────────────┘
                            │
                            ▼
                  chipset-generated address
                            │
                            ▼
                 address masking / decoding
                            │
                            ▼
                   guest physical address
                            │
                            ▼
                     host memory API
                            │
                            ▼
                   Bellatrix guest RAM
~~~

Provider selection remains outside Rigel:

~~~text
M68K address
     │
     ▼
Bellatrix / Emu68
address dispatcher
     │
     ├── native provider
     │
     ├── Rigel provider
     │       │
     │       ▼
     │   public Rigel MMIO API
     │
     └── unmapped
~~~

The MMIO transaction itself remains observable:

~~~text
M68K execution
      │
      ▼
address + width + value + ordering
      │
      ▼
Bellatrix Rigel adapter
      │
      │ preserve transaction semantics
      ▼
public Rigel MMIO boundary
      │
      ▼
classic hardware behavior
      │
      ▼
defined transaction result
~~~

The transaction-result boundary must preserve the distinction:

~~~text
classic guest-visible behavior
             │
             │ distinct from
             ▼
host integration failure
~~~

The harness uses the same boundary:

~~~text
Harness
   │
   ▼
public Rigel API
   │
   ▼
librigel
   │
   ▼
Harness memory backend
~~~

Advanced facilities remain available without becoming mandatory Bellatrix dependencies:

~~~text
                     librigel
                         │
          ┌──────────────┴──────────────┐
          │                             │
 Host Integration API          Advanced / Tooling API
          │                             │
      Bellatrix                    Harness/debug
                                    inspection
                                    snapshots
                                    bus analysis
~~~

---

# 70. Desired Bellatrix Dependency

The final Bellatrix integration should be understandable from a small amount of code.

Conceptually:

~~~c
rigel = rigel_create(
    &config,
    &host_ops,
    host_context);

rigel_reset(
    rigel,
    RIGEL_RESET_COLD);
~~~

The exact post-create/reset sequence remains subject to the lifecycle semantics selected for Version 1.

MMIO remains deliberately conceptual until its transaction semantics and C representation are frozen:

~~~text
Bellatrix
    │
    │ M68K-visible MMIO transaction
    │
    │ address
    │ width
    │ direction
    │ value
    ▼
  Rigel
    │
    ▼
classic hardware semantics
    │
    ▼
defined transaction result
~~~

The final C interface may use width-specific functions, a generic transaction representation, or another explicit form.

The architectural requirement is the transaction semantics, not a predetermined function shape.

Execution likewise remains deliberately conceptual until the temporal API is frozen:

~~~text
deadline = obtain next Rigel deadline

execute M68K according to
Bellatrix / Emu68 policy

report resulting execution progress to Rigel
~~~

The eventual API may become conceptually:

~~~c
rigel_advance(
    rigel,
    execution_progress);
~~~

or:

~~~c
rigel_advance_to(
    rigel,
    reached_time);
~~~

Interrupt:

~~~c
rigel_ipl = rigel_get_ipl(rigel);

effective_ipl =
    bellatrix_arbitrate_ipl(
        native_ipl,
        rigel_ipl);
~~~

DMA travels in the opposite direction:

~~~text
Rigel chipset logic
       │
       ▼
chipset-generated address
       │
       ▼
Rigel address masking / decoding
       │
       ▼
guest physical address
       │
       ▼
host_ops.mem_read/write
       │
       ▼
Bellatrix guest-memory backend
~~~

If the basic Bellatrix adapter requires substantially more knowledge about Rigel than this conceptual model implies, the boundary should be reviewed.

---

# 71. Recommended Implementation Sequence

The concrete work should proceed approximately as follows:

~~~text
0. Capture behavioral baseline
        │
1. Inventory current exported Rigel API
        │
2. Classify every exported symbol
        │
3. Define minimal host integration API
        │
4. Separate config from host operations
        │
5. Formalize guest physical memory semantics
        │
6. Define memory callback failure semantics
        │
7. Formalize Chip RAM topology semantics
        │
8. Introduce canonical MMIO transaction boundary
        │
9. Preserve provider selection outside Rigel
        │
10. Define MMIO width/alignment semantics
        │
11. Define MMIO observability/ordering semantics
        │
12. Define MMIO transaction result/failure semantics
        │
13. Select concrete MMIO C representation
        │
14. Preserve existing internal MMIO handlers
        │
15. Freeze execution-progress unit
        │
16. Freeze deadline representation
        │
17. Select advance vs advance_to semantics
        │
18. Validate authoritative Rigel timeline
        │
19. Define lifecycle/reset semantics
        │
20. Formalize IPL host boundary
        │
21. Separate advanced/debug API
        │
22. Remove internal wiring from host surface
        │
23. Move harness to candidate API
        │
24. Replay behavioral baseline
        │
25. Implement Bellatrix adapter against candidate API
        │
26. Validate deterministic DMA/MMIO/timing/reset/IRQ
        │
27. Review candidate API using both consumers
        │
28. Freeze RIGEL_API_VERSION 1
        │
29. Add advanced bus integration if required
        │
30. Optimize only after correctness
~~~

---

# 72. Definition of Done for Rigel API Version 1

Rigel API Version 1 should not be considered frozen until:

* the existing implementation has a behavioral characterization baseline;
* `librigel` builds independently;
* the standalone harness uses the production API;
* Bellatrix uses only the defined host integration API;
* both harness and Bellatrix have exercised the candidate API;
* Rigel hardware state is opaque;
* hardware configuration and host services have clear ownership;
* guest-memory callback semantics are explicit;
* memory callback failure semantics are explicit;
* Chip RAM configuration does not imply allocation ownership;
* canonical MMIO transaction semantics are explicit;
* provider selection remains outside Rigel;
* supported MMIO widths are explicit;
* MMIO alignment and misalignment behavior is explicit;
* wider-access decomposition semantics are explicit where applicable;
* MMIO transaction observability and ordering semantics are explicit;
* MMIO side effects cannot be accidentally removed by host or execution-engine optimization;
* permitted MMIO transaction transformations, if any, are explicitly documented;
* MMIO transaction result/failure semantics are explicit;
* guest-visible MMIO behavior remains distinct from host integration failure;
* the concrete C representation of MMIO is selected only after its semantics are defined;
* MMIO addresses and guest physical addresses remain distinct;
* chipset-generated addresses and guest physical addresses remain semantically distinct;
* MMIO endianness is explicit;
* execution-progress representation is explicit;
* deadline representation is explicit;
* advancement semantics are explicitly delta-based or absolute;
* Rigel is the sole authoritative chipset timeline;
* overshoot behavior is deterministic;
* post-create state is explicitly defined;
* cold-reset semantics are explicitly defined;
* warm-reset semantics are explicitly defined;
* reset does not transfer chipset reset semantics into Bellatrix;
* Rigel IPL is sufficient for normal Bellatrix interrupt integration;
* interrupt acceptance does not implicitly clear Rigel sources;
* DMA/JIT interaction does not expose Emu68 internals to Rigel;
* callbacks have defined reentrancy rules;
* threading assumptions are documented;
* advanced/debug facilities are distinguishable from the core host API;
* deterministic harness tests pass;
* the Phase 0 behavioral baseline has been replayed after convergence;
* any behavioral differences are documented as intentional;
* Version 1 explicitly states whether it guarantees source API compatibility only or binary ABI compatibility;
* `CONFIG_RIGEL=n` remains independent from the compatibility layer.

Only then should:

~~~c
#define RIGEL_API_VERSION 1
~~~

represent a stable architectural commitment.

For the initial Version 1 release, the recommended compatibility promise is:

> Stable public source-level API. Binary ABI stability is not implied unless separately documented.

---

# 73. Relationship to the Integration Specification

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
migration of the existing implementation
      │
      ▼
include/rigel/rigel.h
      │
      ▼
concrete public API
      │
      ▼
librigel
~~~

This document does not redefine `Rigel_integration.md`.

Its purpose is to explain how the existing Rigel implementation should converge toward that contract.

If a conflict exists:

~~~text
Rigel API Convergence Plan
            │
            ▼
Rigel_integration.md
            │
            ▼
Bellatrix.md
~~~

the higher-level specification takes precedence.

The concrete C interface must therefore be a consequence of these documents rather than a source of new architectural rules.

---

# 74. Final Recommendation

The current Rigel implementation should be treated as the foundation of `librigel`, not as a prototype to be discarded.

Its existing:

* chipset implementation;
* deterministic model;
* temporal model;
* interrupt model;
* DMA behavior;
* bus infrastructure;
* harness;

should be preserved wherever they already conform to the architectural contract.

The primary transformation is:

> from a broad implementation-oriented public surface to a deliberate host-integration API.

The intended result is:

~~~text
                    Rigel internals
                          │
                          │ remain owned by Rigel
                          ▼
                       librigel
                          │
                 candidate public API
                          │
             ┌────────────┴────────────┐
             │                         │
          Harness                   Bellatrix
             │                         │
             └────────────┬────────────┘
                          │
                  integration validation
                          │
                          ▼
                    API Version 1
~~~

Bellatrix must not become aware of how Rigel implements the Amiga chipset.

Rigel must not become aware of how Bellatrix implements the native platform.

The API between them should express only the information that genuinely crosses that architectural boundary.

The guiding rules for the refactoring are therefore:

> Do not rewrite the chipset to fit the API.

> Refine the API so that the existing chipset can remain independently owned, independently tested, and cleanly hosted.

> Establish the existing behavioral baseline before changing the boundary.

> Treat MMIO as an observable hardware transaction boundary rather than ordinary memory access.

> Define MMIO transaction semantics before selecting convenience entry points as stable API.

> Keep guest-visible MMIO hardware behavior distinct from host integration failure.

> Select the concrete C representation of MMIO only after width, alignment, ordering, side-effect, and result semantics are understood.

> Preserve the distinction between chipset-generated addresses and guest physical addresses.

> Preserve the semantic temporal model independently from the eventual choice between `advance()` and `advance_to()`.

> Define lifecycle and reset semantics before freezing reset operations as stable API.

> Validate the candidate interface with both the standalone harness and Bellatrix before freezing Version 1.

Once that boundary is established and validated by both real consumers, the resulting interface can be frozen as Rigel API Version 1.
