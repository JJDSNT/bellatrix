# Rigel / Bellatrix Performance Optimization Candidates

## Possible Optimization Directions for the Emu68 / Bellatrix / Rigel Boundary

**Status:** Investigation Notes  
**Scope:** Performance opportunities only  
**Related:** `Rigel API Convergence Plan`

---

# 1. Purpose

This document records possible performance optimization opportunities for the Emu68 / Bellatrix / Rigel integration.

It does not define required architecture.

It does not require any particular optimization to be implemented.

The purpose is simply to identify areas worth investigating, measuring, and potentially optimizing after the reference integration is correct.

The general principle is:

> **Preserve Rigel as the sole authority for classic hardware semantics while minimizing unnecessary transitions through expensive generic paths.**

A second important principle is:

> **Fault avoidance and shadow state are separate concepts.**

An Emu68 optimization may avoid an MMU fault without requiring Emu68 to maintain a duplicate representation of Rigel-owned hardware state.

Where possible, the preferred optimization model is:

~~~text
Emu68 recognizes operation
          │
          ▼
direct optimized route
          │
          ▼
        Rigel
~~~

rather than:

~~~text
Emu68 recognizes operation
          │
          ▼
local hardware shadow
          │
          ▼
shadow synchronization
          │
          ▼
        Rigel
~~~

The existing Emu68 `INT_shadow` mechanism is therefore interesting primarily as evidence of an existing specialized/fault-avoidance path.

Preserving the shadow itself is not currently an optimization requirement.

---

# 2. Direct Guest Memory Mapping

Ordinary guest memory should remain directly accessible by the M68K CPU wherever possible.

Potential candidates include:

* Chip RAM;
* Slow RAM;
* ROM;
* other ordinary guest RAM.

Conceptually:

~~~text
M68K
  │
  ▼
Emu68
  │
  ▼
direct mapping
  │
  ▼
guest memory
~~~

These accesses should not require:

~~~text
MMU fault
    │
    ▼
Bellatrix Bus
    │
    ▼
Rigel MMIO
~~~

unless there is a specific semantic reason for interception.

---

# 3. Shared Chip RAM Backing

Chip RAM is accessed from two different domains:

~~~text
CPU side

M68K
  │
  ▼
direct Chip RAM mapping
~~~

and:

~~~text
chipset side

Rigel
  │
  ▼
DMA
  │
  ▼
Chip RAM
~~~

A possible optimization is to ensure that both paths operate on the same underlying guest-memory backing without unnecessary copies.

Areas worth investigating include:

* direct shared backing;
* elimination of intermediate buffers;
* efficient address translation;
* avoiding redundant coherency operations.

The desired relationship is:

~~~text
                    Chip RAM backing
                    ▲              ▲
                    │              │
                    │              │
                 M68K CPU       Rigel DMA
               direct access    host memory
~~~

CPU access to Chip RAM is not Rigel MMIO.

Rigel DMA access to Chip RAM is not an Emu68 CPU fault.

---

# 4. Bulk DMA Memory Access

Rigel DMA may generate many small guest-memory accesses.

For example:

~~~text
Copper fetches

Blitter accesses

bitplane fetches

sprite fetches

Paula audio fetches
~~~

If each operation requires a separate host callback, callback overhead may become significant.

Possible optimization:

~~~text
Rigel
  │
  ▼
request contiguous guest-memory span
  │
  ▼
operate directly over validated range
~~~

instead of repeatedly performing:

~~~text
mem_read16()

mem_read16()

mem_read16()

mem_read16()
~~~

Possible approaches include:

* block reads/writes;
* validated memory spans;
* temporary direct mappings;
* cached Chip RAM base information;
* specialized contiguous-memory access.

Any such optimization must preserve DMA visibility and memory coherency.

---

# 5. Pre-Resolved MMIO Routing

The generic Bellatrix Bus may otherwise need to determine the target provider for every intercepted access.

A possible optimization is to classify regions in advance.

Conceptually:

~~~text
address/page
     │
     ▼
pre-resolved provider
     │
     ▼
Rigel
~~~

rather than:

~~~text
address
   │
   ▼
generic provider lookup
   │
   ▼
region search
   │
   ▼
provider selection
   │
   ▼
Rigel
~~~

Possible cached information includes:

* provider;
* region type;
* Rigel instance;
* read handler;
* write handler;
* access capabilities.

---

# 6. MMIO Classification Cache

Emu68 or Bellatrix could potentially cache the classification of frequently accessed addresses or pages.

For example:

~~~text
$DFFxxx
    │
    ▼
RIGEL_CUSTOM_MMIO
~~~

~~~text
$BFDxxx
    │
    ▼
RIGEL_CIA_MMIO
~~~

The cache should represent:

~~~text
routing information
~~~

rather than:

~~~text
cached hardware results
~~~

because MMIO reads and writes may have observable side effects.

---

# 7. Specialized MMIO Fast Paths

Frequently accessed Rigel registers may justify specialized routing paths.

Conceptually:

~~~text
M68K access
     │
     ▼
Emu68 recognizes operation
     │
     ▼
pre-resolved Bellatrix/Rigel path
     │
     ▼
canonical Rigel semantics
~~~

instead of:

~~~text
M68K access
     │
     ▼
MMU fault
     │
     ▼
fault reconstruction
     │
     ▼
Bellatrix Bus
     │
     ▼
generic provider lookup
     │
     ▼
Rigel
~~~

The optimization should preferably eliminate routing overhead rather than duplicate hardware behavior.

Candidates should be identified through profiling rather than assumed in advance.

---

# 8. Investigate the Existing `INT_shadow` Fault-Avoidance Path

The existing Emu68 `INT_shadow` implementation should be investigated carefully.

The important question is not initially:

~~~text
How can Bellatrix preserve INT_shadow?
~~~

The more useful question is:

~~~text
What mechanism allows the existing
INTENA / INTREQ path to avoid the
generic MMU fault?
~~~

These are separate concerns:

~~~text
FAULT AVOIDANCE
      │
      ▼
specialized Emu68 recognition/routing


SHADOW STATE
      │
      ▼
INT_shadow
~~~

Bellatrix may benefit from the first without requiring the second.

The investigation should determine:

* where `INTENA` and `INTREQ` are recognized;
* whether recognition occurs before the generic fault path;
* which accesses actually avoid faults;
* whether the fault avoidance depends intrinsically on `INT_shadow`;
* whether the specialized path can invoke Bellatrix/Rigel directly;
* whether the shadow can be removed from Bellatrix builds while retaining the optimized access path.

---

# 9. Prefer Direct Rigel Fast Routing Over Interrupt Shadow Duplication

If Emu68 can recognize an `INTENA` or `INTREQ` access without generating a generic MMU fault, the preferred Bellatrix optimization candidate is:

~~~text
M68K INTENA / INTREQ access
             │
             ▼
      Emu68 recognition
             │
             ▼
      Bellatrix fast hook
             │
             ▼
   canonical/direct Rigel write
             │
             ▼
           Rigel
~~~

For example:

~~~text
M68K write $DFF09A
        │
        ▼
Emu68 recognizes INTENA
        │
        ▼
no generic MMU fault
        │
        ▼
Bellatrix optimized route
        │
        ▼
rigel_write16($DFF09A, value)
        │
        ▼
Rigel INTENA semantics
~~~

This avoids:

~~~text
MMU fault

fault exception handling

fault reconstruction

generic Bellatrix Bus dispatch

provider lookup
~~~

without introducing a second interrupt-state authority.

---

# 10. `INT_shadow` Is Not Required for Fault Avoidance

The integration should not assume:

~~~text
avoid fault
    =
maintain INT_shadow
~~~

The preferred possibility to investigate is:

~~~text
avoid fault
    =
recognize operation early
    +
route directly to Rigel
~~~

Therefore:

> **Preserving the existing Emu68 fault-avoidance mechanism may be valuable. Preserving `INT_shadow` itself is not a requirement.**

If the shadow exists only because of PiStorm-specific hardware integration requirements, Bellatrix may not need it.

---

# 11. Avoid Duplicate Interrupt State

Rigel already owns:

~~~text
INTENA

INTREQ

interrupt sources

interrupt priority resolution

classic IPL
~~~

Maintaining another authoritative or continuously synchronized representation in Emu68 creates additional complexity:

~~~text
Rigel INTENA/INTREQ
         │
         ↕
 synchronization
         ↕
Emu68 INT_shadow
~~~

This becomes particularly important because `INTREQ` changes can originate internally inside Rigel:

~~~text
VBlank ─────┐
CIA ────────┤
Paula ──────┤
Copper ─────┤
Blitter ────┤
            ▼
          Rigel
            │
            ▼
          INTREQ
~~~

A shadow would therefore require coherence not only for CPU writes but also for Rigel-originated state transitions.

If the optimized Emu68 path can route directly to Rigel, avoiding this duplicated state may itself be a performance and complexity optimization.

---

# 12. Conditional Shadow Retention

`INT_shadow` should not be removed merely for architectural purity if measurements demonstrate that it provides an optimization that cannot be retained otherwise.

The decision should therefore be empirical.

Possible outcomes include:

~~~text
A. specialized path requires INT_shadow
             │
             ▼
retain and synchronize shadow
~~~

~~~text
B. specialized path can call Rigel directly
             │
             ▼
remove/bypass shadow in Bellatrix
~~~

~~~text
C. shadow provides additional measurable benefit
             │
             ▼
evaluate cost of synchronization
against performance gain
~~~

The preferred implementation is the simplest model that preserves the useful fault-avoidance behavior.

---

# 13. IPL Change-Driven Updates

Rigel IPL should ideally not require expensive recomputation or synchronization after every CPU execution block.

Potential model:

~~~text
interrupt-relevant event
        │
        ▼
Rigel updates interrupt state
        │
        ▼
IPL changes?
      /     \
    yes      no
     │        │
     ▼        ▼
 publish    nothing
 new IPL
~~~

Interrupt-relevant events include:

* `INTENA` changes;
* `INTREQ` changes;
* device-generated interrupt requests;
* interrupt clearing;
* reset.

`rigel_get_ipl()` could then remain a cheap read of already-resolved state.

---

# 14. Avoid Excessive IPL Polling

A potential performance problem would be:

~~~text
execute JIT block
      │
      ▼
rigel_get_ipl()

execute JIT block
      │
      ▼
rigel_get_ipl()

execute JIT block
      │
      ▼
rigel_get_ipl()
~~~

Possible alternatives include:

* change notification;
* dirty state;
* cached resolved IPL;
* checking only at required synchronization boundaries.

The important distinction is:

~~~text
CPU writes INTENA / INTREQ
           │
           ▼
          Rigel


Rigel resolves classic IPL
           │
           ▼
       Emu68 INT.IPL
~~~

Rigel does not need to receive an IPL from Emu68.

It receives state-changing hardware transactions and produces the resolved classic IPL.

---

# 15. Cached Resolved IPL

Rigel may benefit from keeping the currently resolved IPL as derived state.

Conceptually:

~~~text
INTENA / INTREQ changes
          │
          ▼
recalculate priority
          │
          ▼
cached resolved IPL
~~~

Then:

~~~text
rigel_get_ipl()
      │
      ▼
cheap state read
~~~

rather than recomputing interrupt priority on every query.

---

# 16. Deadline-Based Rigel Advancement

Rigel should avoid requiring synchronization after every M68K instruction or very small execution quantum.

Preferred direction:

~~~text
Rigel
  │
  ▼
next deadline
  │
  ▼
Emu68 executes useful work
  │
  ▼
deadline reached
  │
  ▼
rigel_advance()
~~~

instead of:

~~~text
CPU executes
    │
rigel_advance()
    │
CPU executes
    │
rigel_advance()
    │
CPU executes
    │
rigel_advance()
~~~

The goal is to reduce host/Rigel boundary crossings while preserving chipset timing.

---

# 17. Larger Safe Execution Quanta

The deadline mechanism may permit Emu68 to execute larger batches of translated code when Rigel guarantees that no externally relevant chipset event occurs before a known point.

Conceptually:

~~~text
current time
     │
     │   safe CPU execution window
     │────────────────────────────►
                                  │
                                  ▼
                            Rigel deadline
~~~

This may reduce:

* synchronization calls;
* IPL checks;
* scheduler transitions;
* host/Rigel API calls.

The maximum safe quantum must remain constrained by observable chipset timing.

---

# 18. JIT MMIO Fast Paths

A more aggressive optimization could allow Emu68 JIT translation to recognize constant MMIO addresses.

For example:

~~~text
MOVE.W D0,$DFF096
~~~

could potentially become conceptually:

~~~text
translated ARM code
       │
       ▼
pre-resolved Rigel write
~~~

instead of:

~~~text
translated ARM code
       │
       ▼
memory operation
       │
       ▼
MMU fault
       │
       ▼
fault reconstruction
       │
       ▼
Bellatrix Bus
       │
       ▼
Rigel
~~~

This could be particularly useful for frequently accessed fixed hardware registers.

Importantly, this optimization does not require Emu68 to implement `DMACON` semantics.

The desired model remains:

~~~text
Emu68 recognizes address
        │
        ▼
Rigel executes semantics
~~~

---

# 19. Generalize Fault Avoidance Beyond `INTENA` / `INTREQ`

If investigation of the existing Emu68 interrupt path reveals a reusable mechanism for avoiding faults, it may be applicable to other frequently accessed Rigel registers.

Conceptually:

~~~text
MOVE.W D0,$DFF096
        │
        ▼
Emu68 recognizes Rigel MMIO
        │
        ▼
direct Bellatrix/Rigel path
        │
        ▼
rigel_write16()
~~~

The same model could potentially apply to other hot constant-address accesses.

This would turn the PiStorm interrupt optimization investigation into a more general:

~~~text
Rigel MMIO fast-path investigation
~~~

rather than an `INT_shadow`-specific feature.

---

# 20. JIT Provider Binding

A translated block could potentially remember that a constant address belongs to a specific provider.

Conceptually:

~~~text
JIT block

$DFF096
   │
   ▼
known Rigel target
   │
   ▼
direct Rigel path
~~~

This would avoid repeated address classification.

Such binding would require an invalidation strategy if the relevant mapping or provider changes.

---

# 21. Stable Classic MMIO Regions

Some classic MMIO mappings may remain stable for the lifetime of the machine.

Examples may include:

~~~text
custom register space

CIA register space
~~~

Stable regions are particularly attractive candidates for:

* pre-resolution;
* JIT classification;
* direct handler binding;
* elimination of repeated provider lookup;
* fault avoidance.

The exact set of stable regions should be determined by Bellatrix machine policy.

---

# 22. Fast Path / Generic Path Separation

The implementation may benefit from explicitly distinguishing:

~~~text
FAST PATH

frequent
pre-classified
low-overhead
~~~

from:

~~~text
GENERIC PATH

complete
flexible
diagnostic
fallback
~~~

Conceptually:

~~~text
                   M68K access
                       │
                       ▼
                 known fast path?
                    /      \
                  yes       no
                   │         │
                   ▼         ▼
             direct route   MMU fault
                   │         │
                   │    Bellatrix Bus
                   │         │
                   └────┬────┘
                        ▼
                      Rigel
~~~

The generic path should optimize for completeness.

Hot paths may optimize for bypassing generic overhead.

Neither path should duplicate Rigel hardware semantics.

---

# 23. Fast Path and Shadow State Must Remain Separate Concepts

Future implementation work should explicitly distinguish:

~~~text
FAST PATH
    =
optimized route to authoritative semantics
~~~

from:

~~~text
SHADOW
    =
cached/duplicated derived state
~~~

A fast path does not inherently require a shadow.

For Bellatrix, the preferred model is generally:

~~~text
M68K
 │
 ▼
Emu68 fast recognition
 │
 ▼
Rigel
~~~

rather than:

~~~text
M68K
 │
 ▼
Emu68 fast recognition
 │
 ▼
Emu68 hardware shadow
 │
 ▼
synchronization
 │
 ▼
Rigel
~~~

unless the latter provides a demonstrated performance advantage.

---

# 24. Reduce Cross-Layer Calls

The integration should measure how often execution crosses boundaries such as:

~~~text
Emu68
  │
  ▼
Bellatrix
  │
  ▼
Rigel
~~~

Potential optimization targets include:

* fewer function calls;
* fewer indirect calls;
* fewer callbacks;
* batching;
* pre-resolved handlers;
* inlineable narrow interfaces;
* reduced argument reconstruction.

This should be driven by profiling.

---

# 25. Canonical MMIO May Already Be a Suitable Fast Target

Before introducing specialized Rigel APIs, the implementation should measure the cost of calling the existing canonical MMIO operation directly.

For example:

~~~text
Emu68 fast recognition
        │
        ▼
rigel_write16(address, value)
        │
        ▼
Rigel
~~~

may already be sufficiently cheap.

This would allow both:

~~~text
generic path
    │
    ▼
rigel_write16()
~~~

and:

~~~text
fast path
    │
    ▼
rigel_write16()
~~~

to share exactly the same semantic implementation.

Only the route to that implementation would differ.

---

# 26. Narrow Fast-Path APIs

If profiling demonstrates that canonical MMIO dispatch itself becomes significant, narrowly defined optimized Rigel entry points may be considered.

Conceptually:

~~~text
generic:

rigel_write16(address, value)
~~~

versus a possible internal optimized equivalent:

~~~text
pre-resolved Rigel operation
        │
        ▼
register implementation
~~~

The public API should not be expanded prematurely.

The preferred sequence is:

~~~text
first:
    bypass fault and Bus

then measure:
    canonical Rigel MMIO cost

only then:
    consider specialized Rigel entry
~~~

---

# 27. Avoid Duplicate Address Translation

The integration should investigate whether the same address is unnecessarily translated multiple times.

For example:

~~~text
M68K address
     │
     ▼
Emu68 translation
     │
     ▼
Bellatrix translation
     │
     ▼
Rigel translation
     │
     ▼
guest memory
~~~

For known memory classes, it may be possible to reduce redundant translation stages.

This is particularly relevant for high-frequency DMA and Chip RAM operations.

---

# 28. Avoid Unnecessary Memory Copies

Potential copy points should be inventoried.

Particular attention should be given to:

* Chip RAM;
* video output;
* audio buffers;
* DMA buffers;
* host presentation buffers.

Where possible:

~~~text
producer
   │
   ▼
shared/owned buffer
   │
   ▼
consumer
~~~

may be preferable to:

~~~text
producer
   │
   ▼
buffer A
   │
   ▼
copy
   │
   ▼
buffer B
   │
   ▼
consumer
~~~

provided ownership and lifetime remain explicit.

---

# 29. Video Batching

Classic video generation may involve high-frequency internal operations.

Possible optimization areas include:

* scanline batching;
* dirty-line tracking;
* dirty-region tracking;
* avoiding regeneration of unchanged output;
* efficient Chip RAM fetch;
* minimizing host presentation transitions.

Any optimization must preserve timing-visible chipset behavior where required.

---

# 30. Audio Batching

Paula audio may benefit from generating samples in blocks rather than performing excessive host transitions for individual samples or very small units.

Potential model:

~~~text
Rigel Paula
     │
     ▼
generate audio block
     │
     ▼
host audio buffer
~~~

The block size must balance:

* latency;
* timing accuracy;
* callback overhead.

---

# 31. Dirty-State Tracking

Rigel subsystems may benefit from explicit dirty-state tracking where derived state is expensive to recompute.

Examples may include:

~~~text
interrupt result dirty

video output dirty

routing cache dirty

presentation state dirty
~~~

Shadow-specific dirty state should only exist if a shadow is actually retained.

The implementation should not introduce shadow synchronization infrastructure merely because the PiStorm implementation currently contains a shadow.

---

# 32. Generation Counters

Generation counters may be useful where multiple cached or derived representations genuinely exist.

Conceptually:

~~~text
Rigel state generation = N

cached state generation = N
        │
        ▼
cache valid
~~~

After a relevant change:

~~~text
Rigel state generation = N + 1

cached state generation = N
        │
        ▼
refresh required
~~~

This may provide a cheap coherency mechanism for selected optimization state.

It should not be introduced where direct authoritative access is cheaper.

---

# 33. Avoid Premature Fine-Grained Optimization

Not every MMIO register requires a specialized path.

The recommended sequence is:

~~~text
correct generic implementation
        │
        ▼
instrumentation
        │
        ▼
profiling
        │
        ▼
identify hot paths
        │
        ▼
specialize only where useful
~~~

Optimization should be driven by observed cost.

---

# 34. Instrumentation

The integration should provide enough instrumentation to measure:

* number of MMU faults;
* number of Rigel-related faults;
* MMIO accesses by region;
* MMIO accesses by register;
* Bellatrix Bus dispatch count;
* direct Rigel fast-path count;
* fast-path hit count;
* fast-path miss count;
* canonical MMIO calls;
* `INTENA` accesses;
* `INTREQ` accesses;
* existing Emu68 `INT_shadow` path hits during investigation;
* faults avoided by specialized Emu68 paths;
* DMA memory operations;
* DMA bytes transferred;
* `rigel_advance()` calls;
* average CPU progress per `rigel_advance()`;
* IPL changes;
* IPL polls;
* JIT invalidations;
* video output operations;
* audio output operations.

Without these measurements, optimization priorities will remain speculative.

---

# 35. Fault Cost Measurement

The complete cost of a Rigel-related fault should be measured.

Conceptually:

~~~text
M68K access
     │
     ▼
MMU fault
     │
     ▼
exception entry
     │
     ▼
fault reconstruction
     │
     ▼
address classification
     │
     ▼
Bellatrix Bus
     │
     ▼
provider lookup
     │
     ▼
Rigel MMIO
     │
     ▼
return
~~~

This provides the baseline against which fast paths should be evaluated.

A direct path can then be compared against:

~~~text
M68K access
     │
     ▼
Emu68 recognition
     │
     ▼
direct Rigel MMIO
     │
     ▼
return
~~~

---

# 36. `INT_shadow` Investigation Benchmark

The existing Emu68 path should be benchmarked in terms of its individual components.

The investigation should distinguish:

~~~text
cost avoided by specialized recognition

cost avoided by bypassing the MMU fault

cost avoided by INT_shadow itself
~~~

This distinction is important.

It may reveal that most of the performance advantage comes from:

~~~text
early recognition + fault bypass
~~~

rather than:

~~~text
duplicated interrupt state
~~~

If so, Bellatrix should preserve the former without necessarily preserving the latter.

---

# 37. Optimization Priority Candidates

Initial candidates worth measuring include:

~~~text
1. Direct guest-memory mapping

2. Shared Chip RAM backing

3. Pre-resolved MMIO routing

4. Existing Emu68 fault-avoidance mechanism

5. Direct Rigel MMIO fast routing

6. Cached/resolved IPL

7. Deadline-based Rigel advancement

8. DMA batching / direct memory spans

9. MMIO classification cache

10. Specialized hot-register routing

11. JIT MMIO fast paths
~~~

`INT_shadow` itself is deliberately not listed as an optimization objective.

Instead, it is an implementation mechanism to investigate in order to determine which useful optimization properties can be reused.

This ordering is not normative.

Profiling should determine the actual implementation priority.

---

# 38. Possible Long-Term Execution Model

A highly optimized implementation might eventually resemble:

~~~text
                         M68K
                           │
                           ▼
                         Emu68
                           │
          ┌────────────────┼─────────────────┐
          │                │                 │
          ▼                ▼                 ▼
    direct memory      JIT/MMIO          generic
       mapping          fast path         fallback
          │                │                 │
          ▼                │             MMU fault
   Chip/Slow/ROM           │                 │
                           │          Bellatrix Bus
                           │                 │
                           └────────┬────────┘
                                    ▼
                                  Rigel
                                    │
                 ┌──────────────────┼──────────────────┐
                 │                  │                  │
                 ▼                  ▼                  ▼
               timing             DMA                IPL
                 │                  │                  │
                 ▼                  ▼                  ▼
             deadlines         guest memory        INT.IPL
~~~

For interrupt-register writes:

~~~text
M68K
 │
 │ INTENA / INTREQ
 ▼
Emu68
 │
 │ specialized recognition
 ▼
Bellatrix fast route
 │
 ▼
Rigel canonical MMIO
 │
 ├── INTENA
 ├── INTREQ
 └── priority resolution
        │
        ▼
     Rigel IPL
        │
        ▼
   Emu68 INT.IPL
~~~

No Emu68 interrupt shadow is inherently required by this model.

The generic path remains:

~~~text
M68K
 │
 ▼
unoptimized access
 │
 ▼
MMU fault
 │
 ▼
Bellatrix Bus
 │
 ▼
Rigel canonical MMIO
~~~

The common paths progressively avoid unnecessary generic overhead without moving classic hardware state out of Rigel.

---

# 39. Optimization Invariants

Any optimization should preserve:

~~~text
Rigel hardware authority

single authoritative classic hardware state

M68K-visible behavior

MMIO side effects

transaction width

transaction ordering

interrupt semantics

DMA semantics

chipset timing

memory coherency

generic fallback
~~~

Performance optimization must not create a second implementation of classic hardware semantics inside Bellatrix or Emu68.

In particular:

~~~text
fault avoidance
    !=
hardware-state duplication
~~~

and:

~~~text
fast path
    !=
shadow
~~~

---

# 40. Investigation Sequence for the Emu68 Interrupt Path

Before deciding whether `INT_shadow` belongs in Bellatrix, the existing implementation should be decomposed experimentally.

Recommended investigation:

~~~text
1. Locate INT_shadow accesses.

2. Locate INTENA / INTREQ recognition.

3. Determine whether recognition occurs
   before the generic fault handler.

4. Determine exactly which accesses
   avoid faults.

5. Determine what role INT_shadow
   actually plays in fault avoidance.

6. Separate:
      access recognition
      fault bypass
      shadow storage
      interrupt calculation
      CPU IPL delivery

7. Determine whether Bellatrix can reuse:
      recognition + fault bypass

   without reusing:
      shadow state

8. Route the recognized operation
   directly to Rigel.

9. Compare behavior against
   canonical fault/Bus routing.

10. Benchmark both paths.

11. Retain INT_shadow only if it provides
    an additional demonstrated benefit.
~~~

This avoids making the current PiStorm implementation structure an architectural requirement for Bellatrix.

---

# 41. Guiding Principle

The optimization strategy can be summarized as:

> **Keep the generic path complete, then make common operations reach Rigel more directly where doing so is measurably useful and semantically safe.**

For memory:

~~~text
M68K
 │
 ▼
direct mapping
~~~

For optimized MMIO:

~~~text
M68K
 │
 ▼
Emu68 recognition
 │
 ▼
direct Rigel route
~~~

For generic MMIO:

~~~text
M68K
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

For DMA:

~~~text
Rigel
 │
 ▼
guest memory
~~~

For timing:

~~~text
Emu68 progress
 │
 ▼
Rigel advance/deadline
~~~

For interrupts:

~~~text
CPU INTENA/INTREQ writes
          │
          ▼
         Rigel
          │
          ▼
      resolved IPL
          │
          ▼
     Emu68 INT.IPL
~~~

The target is not:

~~~text
eliminate the generic path
~~~

nor:

~~~text
force everything through
the generic path
~~~

nor:

~~~text
preserve every PiStorm-specific
optimization data structure
~~~

The target is:

~~~text
complete correctness
        +
single Rigel hardware authority
        +
measured specialization
        +
fault avoidance where useful
        +
minimal duplicated state
        +
minimal unnecessary overhead
~~~

The specific lesson from `INT_shadow` is therefore:

> **Investigate and preserve the useful fault-avoidance mechanism, not necessarily the shadow that happens to accompany it in the existing Emu68/PiStorm implementation.**

And the broader optimization direction is:

> **Emu68 should accelerate the path to Rigel rather than duplicate Rigel hardware state.**
