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

> **Preserve Rigel as the authority for classic hardware semantics while minimizing unnecessary transitions through expensive generic paths.**

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

Frequently accessed Rigel registers may justify specialized paths.

Conceptually:

~~~text
M68K access
     │
     ▼
recognized hot register
     │
     ▼
specialized Rigel entry
     │
     ▼
Rigel semantics
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
generic MMIO dispatcher
     │
     ▼
Rigel
~~~

Candidates should be identified through profiling rather than assumed in advance.

---

# 8. Preserve `INT_shadow`

The existing Emu68 `INT_shadow` mechanism should be investigated as a potential fault-avoidance optimization for:

~~~text
INTENA

INTREQ
~~~

Possible desired path:

~~~text
M68K INT access
       │
       ▼
Emu68 specialized path
       │
       ▼
INT_shadow
       │
       ▼
minimal Rigel semantic update
       │
       ▼
Rigel
~~~

The objective would be to avoid the generic MMU fault and Bellatrix Bus path while preserving Rigel ownership of interrupt semantics.

---

# 9. Lazy Shadow Synchronization

If Emu68 shadows must remain coherent with Rigel state, immediate synchronization after every internal Rigel event may not always be necessary.

A possible optimization is:

~~~text
Rigel state changes
       │
       ▼
mark shadow dirty
       │
       ▼
continue execution
       │
       ▼
shadow actually needed
       │
       ▼
refresh
~~~

This should only be used where observable behavior remains correct.

Possible techniques include:

* dirty flags;
* generation counters;
* invalidation;
* lazy refresh;
* derived shadow state.

---

# 10. IPL Change-Driven Updates

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

# 11. Avoid Excessive IPL Polling

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

The appropriate mechanism should be determined from the actual Emu68 execution model.

---

# 12. Deadline-Based Rigel Advancement

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

# 13. Larger Safe Execution Quanta

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

# 14. JIT MMIO Fast Paths

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

---

# 15. JIT Provider Binding

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
direct specialized call
~~~

This would avoid repeated address classification.

Such binding would require an invalidation strategy if the relevant mapping or provider changes.

---

# 16. Stable Classic MMIO Regions

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
* elimination of repeated provider lookup.

The exact set of stable regions should be determined by Bellatrix machine policy.

---

# 17. Fast Path / Generic Path Separation

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
                 known hot path?
                    /      \
                  yes       no
                   │         │
                   ▼         ▼
               fast path   generic path
                   │         │
                   │      MMU fault
                   │         │
                   │    Bellatrix Bus
                   │         │
                   └────┬────┘
                        ▼
                      Rigel
~~~

The generic path should optimize for completeness.

Hot paths may optimize for bypassing generic overhead.

---

# 18. Reduce Cross-Layer Calls

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

# 19. Narrow Fast-Path APIs

If generic canonical MMIO becomes expensive for known hot operations, narrowly defined optimized Rigel entry points may be considered.

Conceptually:

~~~text
generic:

rigel_mmio_write16(address, value)
~~~

versus a possible internal optimized equivalent:

~~~text
pre-resolved operation
        │
        ▼
Rigel register semantics
~~~

The public API should not be expanded prematurely.

Fast-path interfaces should only be introduced after profiling demonstrates a useful benefit.

---

# 20. Avoid Duplicate Address Translation

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

# 21. Avoid Unnecessary Memory Copies

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

# 22. Video Batching

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

# 23. Audio Batching

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

# 24. Dirty-State Tracking

Many Rigel subsystems may benefit from explicit dirty-state tracking.

Examples:

~~~text
interrupt state dirty

video state dirty

shadow dirty

output dirty

routing cache dirty
~~~

This may allow expensive derived-state calculations to occur only when required.

---

# 25. Generation Counters

Generation counters may be useful where multiple cached or derived representations exist.

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

---

# 26. Avoid Premature Fine-Grained Optimization

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

# 27. Instrumentation

The integration should provide enough instrumentation to measure:

* number of MMU faults;
* number of Rigel-related faults;
* MMIO accesses by region;
* MMIO accesses by register;
* Bellatrix Bus dispatch count;
* fast-path hit count;
* fast-path miss count;
* `INT_shadow` hits;
* canonical MMIO calls;
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

# 28. Fault Cost Measurement

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

---

# 29. Optimization Priority Candidates

Initial candidates worth measuring include:

~~~text
1. Direct guest-memory mapping

2. Shared Chip RAM backing

3. Pre-resolved MMIO routing

4. INT_shadow fault avoidance

5. Cached/resolved IPL

6. Deadline-based Rigel advancement

7. DMA batching / direct memory spans

8. MMIO classification cache

9. Specialized hot-register handlers

10. JIT MMIO fast paths
~~~

This ordering is not normative.

Profiling should determine the actual implementation priority.

---

# 30. Possible Long-Term Execution Model

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
    direct memory      JIT MMIO          generic
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

The generic path remains available for correctness and complete coverage.

The common paths progressively avoid unnecessary generic overhead.

---

# 31. Optimization Invariants

Any optimization should preserve:

~~~text
Rigel hardware authority

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

---

# 32. Guiding Principle

The optimization strategy can be summarized as:

> **Keep the generic path complete, then make common operations avoid it where doing so is measurably useful and semantically safe.**

Or:

~~~text
cold / unusual operation
          │
          ▼
generic complete path


hot / predictable operation
          │
          ▼
validated optimized path
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

The target is:

~~~text
complete correctness
        +
measured specialization
        +
minimal unnecessary overhead
~~~
