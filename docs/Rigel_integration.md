# Bellatrix / Rigel Integration Specification

## Host Interface, MMIO, Timing, Interrupts, DMA, Memory, and Lifecycle

**Status:** Architectural Integration Baseline  
**Version:** 1

This document defines the integration contract between Bellatrix and `librigel`.

The architectural responsibilities, ownership rules, and platform definition are established by `Bellatrix.md`.

This document does not redefine those architectural decisions.

Its purpose is to specify how Bellatrix, acting as a Rigel host, attaches the optional classic Amiga hardware compatibility layer to the native M68K platform.

The fundamental dependency direction is:

~~~text
Bellatrix
    │
    │ public Rigel API
    ▼
 librigel
    │
    │ host operations
    ▼
Host-provided services
~~~

Bellatrix MAY depend on the public Rigel interface.

`librigel` MUST NOT depend on Bellatrix internals.

---

# 1. Integration Goals

The Bellatrix/Rigel integration must provide the minimum mechanisms required for Rigel to operate as a complete classic Amiga hardware compatibility component.

The integration boundary consists of:

* lifecycle control;
* MMIO dispatch;
* execution-progress delivery;
* synchronization deadlines;
* interrupt-level delivery;
* guest-memory access for chipset DMA;
* reset propagation;
* host-independent video/audio output;
* host input adaptation;
* optional diagnostic services.

The boundary MUST NOT expose Rigel internals to Bellatrix.

Bellatrix MUST NOT directly operate:

* Agnus;
* Denise;
* Paula;
* CIAA;
* CIAB;
* Copper;
* Blitter;
* beam state;
* chipset DMA scheduling;
* classic interrupt state;
* `INTENA`;
* `INTREQ`.

Those remain exclusively owned by Rigel.

---

# 2. Integration Model

Conceptually:

~~~text
                         Bellatrix
                             │
                    Bellatrix Rigel Adapter
                             │
          ┌──────────────────┼──────────────────┐
          │                  │                  │
         MMIO             Progress             IPL
          │                  │                  │
          └──────────────────┼──────────────────┘
                             │
                         librigel
                             │
             ┌───────────────┼───────────────┐
             │               │               │
           Agnus           Denise          Paula
             │                               │
          Copper                         CIAA/CIAB
          Blitter
~~~

The Bellatrix adapter is responsible only for translating between the Bellatrix/Emu68 platform boundary and the public Rigel API.

It MUST NOT contain chipset behavior.

---

# 3. Component Boundary

The recommended source-level separation is conceptually:

~~~text
Bellatrix
│
├── platform/
├── emu68/
├── ...
└── rigel_adapter.c
          │
          ▼
      librigel
~~~

The exact Bellatrix directory layout is not normative.

The architectural requirement is that Rigel integration remain isolated behind a small adapter.

A useful conformance test is:

> Removing the Bellatrix Rigel adapter and disabling `CONFIG_RIGEL` must remove the compatibility layer without requiring structural changes elsewhere in the native platform.

Logic involving:

* Copper operation;
* beam interpretation;
* Blitter semantics;
* Paula semantics;
* CIA timers;
* classic DMA scheduling;
* classic register interpretation;

MUST NOT migrate into the Bellatrix adapter.

---

# 4. Public Rigel API

The public API SHOULD expose operations on an opaque Rigel instance.

Conceptually:

~~~c
struct rigel;
struct rigel_config;
struct rigel_host_ops;

struct rigel *
rigel_create(
    const struct rigel_config *config,
    const struct rigel_host_ops *host_ops,
    void *host_context);

void
rigel_destroy(struct rigel *rigel);

void
rigel_reset(
    struct rigel *rigel,
    enum rigel_reset_type type);
~~~

The exact names remain implementation decisions.

The following properties are normative:

* Rigel state MUST be encapsulated.
* Bellatrix MUST NOT require access to internal Rigel structures.
* Host-specific state MUST be supplied through an opaque host context.
* Rigel MUST support independent instantiation by a standalone harness.
* Global Bellatrix-specific state MUST NOT be required by `librigel`.

---

# 5. Minimal Host Operations

The initial host interface SHOULD remain deliberately small.

At minimum, Rigel requires access to the guest-memory backend needed for chipset DMA.

Conceptually:

~~~c
struct rigel_host_ops {
    uint8_t  (*mem_read8)(void *ctx, uint32_t addr);
    uint16_t (*mem_read16)(void *ctx, uint32_t addr);
    uint32_t (*mem_read32)(void *ctx, uint32_t addr);

    void (*mem_write8)(void *ctx, uint32_t addr, uint8_t value);
    void (*mem_write16)(void *ctx, uint32_t addr, uint16_t value);
    void (*mem_write32)(void *ctx, uint32_t addr, uint32_t value);

    /* Optional diagnostic service. */
    void (*log)(void *ctx, int level, const char *message);
};
~~~

This structure is illustrative.

Only callbacks demonstrated to be necessary SHOULD be added.

In particular, the initial interface SHOULD NOT contain a generic:

~~~c
signal_event(...);
~~~

or equivalent catch-all callback.

A generic event callback could become an uncontrolled escape mechanism across the architectural boundary.

Asynchronous host notifications SHOULD be introduced only when a concrete integration requirement establishes their semantics.

---

# 6. Host Context

The host context is opaque to Rigel.

Conceptually:

~~~c
void *host_context;
~~~

Rigel MUST:

* store the value without interpreting it;
* pass it unchanged to host callbacks;
* make no assumptions about its contents.

This allows the same `librigel` implementation to operate with:

* Bellatrix;
* a standalone test harness;
* another emulator;
* another future host.

---

# 7. Callback Reentrancy

Unless explicitly documented otherwise, callbacks invoked by Rigel MUST NOT re-enter the same Rigel instance.

For example:

~~~text
rigel_advance()
      │
      ▼
host mem_read()
      │
      ╳
      └── MUST NOT call rigel_advance()
          on the same instance
~~~

The same restriction applies to Rigel MMIO operations unless a future API explicitly defines safe reentrant behavior.

This rule avoids hidden recursion, partially updated chipset state, complex locking requirements, and nondeterministic ordering.

A future revision MAY relax this restriction for explicitly identified operations.

---

# 8. MMIO Registration

When Rigel is enabled, Bellatrix registers the classic hardware regions handled by Rigel with the Emu68 address/fault infrastructure.

Conceptually:

~~~text
Emu68 address dispatcher
        │
        ├── native mappings
        ├── Rigel custom region
        ├── Rigel CIAA region
        └── Rigel CIAB region
~~~

Bellatrix owns registration of address-space providers.

Rigel owns the semantics of accesses within regions assigned to it.

This distinction is normative.

---

# 9. MMIO Dispatch

An intercepted classic hardware access follows this path:

~~~text
M68K instruction
      │
      ▼
Emu68 MMU / fault handling
      │
      ▼
Bellatrix Rigel adapter
      │
      ▼
public Rigel API
      │
      ▼
librigel
      │
      ▼
classic hardware semantics
~~~

The Bellatrix adapter MAY translate the host representation of an MMIO transaction into the public Rigel API.

It MUST NOT interpret the target chipset register.

For example, Bellatrix may determine:

~~~text
address = 0x00DFF096
width   = 16
type    = WRITE
value   = 0x8200
~~~

but the meaning of that write belongs entirely to Rigel.

Bellatrix MUST NOT contain logic equivalent to:

~~~c
if (addr == DMACON)
    update_dma_state(...);
~~~

Such behavior belongs inside `librigel`.

---

# 10. MMIO Access API

The initial interface may conceptually provide:

~~~c
uint8_t
rigel_read8(struct rigel *rigel, uint32_t addr);

uint16_t
rigel_read16(struct rigel *rigel, uint32_t addr);

uint32_t
rigel_read32(struct rigel *rigel, uint32_t addr);

void
rigel_write8(struct rigel *rigel, uint32_t addr, uint8_t value);

void
rigel_write16(struct rigel *rigel, uint32_t addr, uint16_t value);

void
rigel_write32(struct rigel *rigel, uint32_t addr, uint32_t value);
~~~

The exact supported widths MAY differ by compatibility region.

Unsupported, invalid, or historically unusual access widths MUST have explicitly defined behavior rather than inheriting accidental behavior from the ARM host.

---

# 11. MMIO Address Representation

Addresses supplied through the public MMIO interface SHOULD use one canonical representation.

The preferred model is the M68K-visible physical address:

~~~text
0x00DFFxxx
0x00BFxxxx
...
~~~

rather than caller-dependent register offsets.

This allows the public interface to remain self-describing and able to route multiple compatibility regions consistently.

If region-relative offsets are eventually selected instead, that decision MUST be globally consistent and documented by the public ABI.

---

# 12. Endianness

The M68K-visible hardware model is big-endian.

The host hardware may use a different native byte order.

The integration boundary MUST define where conversion occurs.

The preferred rule is:

> Values crossing the public Rigel MMIO API represent M68K-visible logical values.

Therefore:

~~~c
rigel_write16(rigel, 0x00DFF096, 0x8200);
~~~

means that the M68K guest wrote the logical 16-bit value `0x8200`.

ARM-native byte representation MUST NOT leak into Rigel register semantics.

---

# 13. Unmapped Accesses

Rigel SHOULD receive only accesses to regions registered as belonging to Rigel.

An address outside those regions remains the responsibility of the Bellatrix/Emu68 address dispatcher.

Conceptually:

~~~text
M68K address
     │
     ▼
Dispatcher
     │
     ├── native mapping ─────► native hardware
     │
     ├── Rigel mapping ──────► librigel
     │
     └── no mapping ─────────► normal unmapped behavior
~~~

Rigel MUST NOT become a generic fallback handler for arbitrary M68K addresses.

---

# 14. Autoconfig Provider

The historical Autoconfig address region is not intrinsically owned by Rigel merely because Rigel is enabled.

If classic Autoconfig functionality is required, it must be represented as an explicit compatibility provider.

Conceptually:

~~~text
0xE80000 access
      │
      ▼
Emu68 dispatcher
      │
      ▼
registered provider?
     / \
   yes  no
    │    │
    ▼    ▼
provider unmapped
~~~

Such a provider MAY eventually be implemented by Rigel or by another compatibility component.

That decision is independent from basic classic chipset integration.

`CONFIG_RIGEL=y` MUST NOT automatically imply a virtual Zorro bus.

---

# 15. Execution-Progress Contract

Bellatrix supplies virtual execution progress to Rigel.

Rigel interprets that progress according to classic Amiga hardware timing.

Conceptually:

~~~text
M68K execution
      │
      ▼
virtual execution progress
      │
      ▼
Bellatrix adapter
      │
      ▼
librigel
      │
      ├── beam
      ├── DMA
      ├── Copper
      ├── Blitter
      ├── Paula
      └── CIA
~~~

Bellatrix MUST NOT convert execution progress into chipset-specific concepts.

---

# 16. Progress Unit

The public interface MUST define exactly one canonical execution-progress representation.

Possible implementations include:

~~~c
rigel_advance_cycles(...);
~~~

or:

~~~c
rigel_advance_ns(...);
~~~

The exact representation remains to be selected.

The chosen unit MUST provide:

* deterministic conversion;
* sufficient precision for classic chipset timing;
* efficient use from the Emu68 execution path;
* stable semantics across different hosts;
* no dependency on real-world wall-clock time.

The unit represents virtual execution progress.

It MUST NOT mean elapsed host time.

---

# 17. Canonical Chipset Timeline

Rigel owns its internal chipset timeline.

Bellatrix MAY maintain execution-progress accounting required to synchronize CPU execution with that timeline.

Bellatrix MUST NOT independently maintain authoritative chipset time.

Conceptually:

~~~text
Bellatrix
    │
    │ execution-progress accounting
    ▼
Rigel
    │
    │ authoritative chipset timeline
    ├── beam
    ├── DMA
    ├── Copper
    ├── Blitter
    ├── Paula
    └── CIA
~~~

This distinction is fundamental.

A Bellatrix-side progress counter represents how much execution has been reported or remains to be synchronized.

It does not become a second authoritative representation of:

* beam time;
* E-clock state;
* DMA slot position;
* Copper time;
* Paula time;
* CIA time.

The governing principle remains:

> The CPU reports progress. The chipset owns chipset time.

---

# 18. No Wall-Clock-Driven Chipset

Rigel correctness MUST NOT be driven directly by:

* ARM generic timer wall time;
* host scheduler latency;
* USB timing;
* display refresh timing;
* operating-system timestamps;
* `gettimeofday()`-style sources;
* other real-time host clocks.

Host clocks MAY be used externally for pacing.

They MUST NOT define chipset state.

Conceptually:

~~~text
Virtual execution progress ───► chipset correctness

Host wall clock ──────────────► optional external pacing
~~~

This distinction is required for deterministic execution and reproducible testing.

---

# 19. Progress Delivery

Bellatrix MAY accumulate execution progress before advancing Rigel.

Conceptually:

~~~text
CPU executes
     │
     ▼
progress accumulator
     │
     ├── continue
     │
     └── synchronization required
                │
                ▼
          rigel_advance(...)
~~~

The batching policy belongs to Bellatrix.

However, batching MUST respect synchronization deadlines established by Rigel.

Bellatrix MUST NOT use arbitrary batching that allows CPU execution to proceed indefinitely past a point where Rigel requires an opportunity to update its state.

---

# 20. Synchronization Deadline

Rigel MAY expose the next synchronization deadline beyond which the host MUST NOT advance execution without giving Rigel an opportunity to update its state.

Conceptually:

~~~c
rigel_time_t
rigel_next_deadline(const struct rigel *rigel);
~~~

The exact API name is illustrative.

The semantic contract is more important than the name.

The host MUST NOT need to understand why the deadline exists.

The deadline may internally result from:

* Copper progression;
* beam timing;
* DMA scheduling;
* CIA state;
* Paula state;
* Blitter state;
* interrupt timing;
* another chipset condition.

These reasons remain private to Rigel.

The contract is simply:

~~~text
Rigel
  │
  ▼
next synchronization deadline
  │
  ▼
Host may execute up to that boundary
  │
  ▼
Rigel gets opportunity to advance/re-evaluate
~~~

The public contract SHOULD NOT require the host to classify the event behind the deadline.

---

# 21. Deadline Representation

The public ABI MUST explicitly define how a synchronization deadline is represented.

In particular, it MUST define whether a returned deadline is:

~~~text
absolute virtual timestamp
~~~

or:

~~~text
delta from current Rigel time
~~~

This specification does not mandate either representation.

That choice belongs to the concrete public ABI.

Implementations MUST NOT infer whether a deadline is absolute or relative from call context.

The representation must be explicit, stable, and documented.

---

# 22. Deadline-Based Execution

The preferred execution relationship is deadline based.

Conceptually:

~~~text
Rigel
 │
 │ next synchronization deadline
 ▼
deadline
 │
 ▼
Bellatrix / Emu68 executes
 │
 ▼
reported progress
 │
 ▼
Rigel advances
 │
 ▼
new deadline
~~~

Rigel determines when its internal hardware model next requires synchronization.

Bellatrix determines how CPU execution reaches that point.

This preserves the architectural principle:

> The CPU reports progress. The chipset owns chipset time.

---

# 23. Overshoot

The integration MUST correctly handle execution progress that passes a previously reported Rigel deadline.

For example:

~~~text
Rigel deadline: 100 units

CPU reports:    112 units
~~~

Rigel must be able to process the state transition associated with the deadline and continue deterministically through the remaining progress.

Correctness MUST NOT depend on Bellatrix being able to stop M68K execution at an exact chipset deadline.

However:

> Overshoot support is a correctness requirement, not the preferred scheduling strategy.

Bellatrix SHOULD avoid unnecessarily large overshoot when the execution engine can reasonably stop near the requested synchronization boundary.

The existence of overshoot support MUST NOT be interpreted as permission for arbitrarily large execution batches.

---

# 24. Rigel Interrupt Ownership

Rigel owns classic Amiga interrupt state.

This includes:

* `INTENA`;
* `INTREQ`;
* classic interrupt-source state;
* classic priority resolution.

Bellatrix MUST NOT independently maintain another authoritative implementation of this state.

The CPU-visible output of this subsystem is the currently asserted compatibility-domain M68K IPL.

Conceptually:

~~~text
Chipset events
      │
      ▼
    INTREQ
      │
      ▼
INTREQ & INTENA
      │
      ▼
classic priority resolution
      │
      ▼
   Rigel IPL
~~~

---

# 25. IPL Interface

Rigel SHOULD expose its currently asserted M68K interrupt priority level.

Conceptually:

~~~c
unsigned
rigel_get_ipl(const struct rigel *rigel);
~~~

The returned value represents the CPU-visible interrupt priority asserted by the classic Amiga hardware domain.

Bellatrix does not need to know which internal Rigel source produced that level.

---

# 26. IPL Observation

Bellatrix must be able to observe Rigel IPL transitions at the synchronization points required to preserve CPU-visible M68K interrupt semantics.

This MAY initially be implemented by querying the current IPL after Rigel advancement.

A future implementation MAY introduce a more explicit notification mechanism if justified by performance or scheduling requirements.

No generic asynchronous host-event callback is required by the initial architecture.

The important requirement is:

> Bellatrix must not allow CPU execution to proceed incorrectly beyond a point at which a changed Rigel IPL should have become visible.

---

# 27. Native and Rigel IPL Arbitration

Bellatrix may simultaneously observe:

~~~text
native_ipl
rigel_ipl
~~~

These values belong to independent interrupt domains.

Conceptually:

~~~text
native interrupt domain ───► native_ipl ──┐
                                          │
                                          ▼
                                      arbitration
                                          │
                                          ▼
                                       M68K IPL
                                          ▲
                                          │
Rigel interrupt domain ─────► rigel_ipl ──┘
~~~

The initial arbitration policy is expected to select the highest currently asserted M68K interrupt priority level.

However, the exact delivery, transition, acknowledgement, and re-evaluation protocol is implementation-defined and MUST preserve M68K interrupt semantics.

A simple expression such as:

~~~text
highest(native_ipl, rigel_ipl)
~~~

describes priority selection only.

It does NOT by itself define the complete interrupt-delivery protocol.

The arbitration layer MUST NOT merge the underlying interrupt state.

---

# 28. Interrupt Acceptance and Acknowledgement

CPU acceptance of an interrupt level is not equivalent to acknowledgement or clearing of the originating hardware source.

Normative rule:

~~~text
CPU accepts IPL
      ≠
device source acknowledged
      ≠
INTREQ cleared
~~~

When the M68K accepts an interrupt exception, Bellatrix MUST NOT automatically clear the corresponding Rigel interrupt source.

Classic interrupt-source state remains governed by classic hardware semantics.

Software may subsequently manipulate Rigel-owned registers, or another Rigel-defined hardware condition may change the source state.

Likewise, Rigel MUST NOT acknowledge, mask, or clear native BCM interrupt-controller state.

---

# 29. Interrupt Re-evaluation

When either interrupt domain changes, the effective CPU-visible IPL may need to be re-evaluated.

For example:

~~~text
native_ipl = 2
rigel_ipl  = 4
      │
effective priority = 4

Rigel source disappears
      │
rigel_ipl = 0
      │
effective priority must be re-evaluated
      │
native level 2 may remain asserted
~~~

The implementation MUST preserve this behavior.

The native and compatibility domains remain independent even though their CPU-visible output shares the M68K IPL interface.

---

# 30. Address-Space Model

The integration must distinguish different address representations explicitly.

At minimum, the following concepts MUST remain separate:

~~~text
MMIO register address

Chipset-visible DMA address

Guest physical address

Host memory address / pointer
~~~

They MUST NOT be treated as interchangeable simply because an implementation can sometimes map between them cheaply.

---

# 31. Chipset-Visible Address Translation

For classic chipset DMA, the conceptual translation is:

~~~text
Chipset-visible address
        │
        ▼
Rigel address semantics
        │
        ▼
Guest physical address
        │
        ▼
Host memory backend
        │
        ▼
Host pointer / physical RAM
~~~

Rigel owns the interpretation of chipset-visible addresses.

Bellatrix owns the host representation and mapping of guest physical memory.

This distinction applies regardless of whether the eventual implementation uses callbacks or optimized direct memory windows.

---

# 32. Chip RAM Ownership

Bellatrix owns guest physical memory allocation and mapping.

Rigel owns the chipset-visible interpretation of the subset of guest memory exposed as Chip RAM.

Normative distinction:

~~~text
Bellatrix
    owns:
        guest physical allocation
        guest physical mapping
        host backing storage

Rigel
    owns:
        what the chipset can see
        chipset pointer semantics
        Agnus-visible address rules
        Chip RAM DMA interpretation
~~~

Rigel MUST NOT become the general allocator of Bellatrix guest memory.

Bellatrix MUST NOT reproduce Agnus or other chipset-specific address-generation rules merely to provide memory to Rigel.

---

# 33. Chip RAM Configuration Semantics

Configuration fields such as:

~~~c
uint32_t chip_ram_size;
~~~

describe the chipset-visible memory topology presented to Rigel.

They do not transfer guest-memory allocation or mapping ownership to Rigel.

Normative rule:

> `chip_ram_size` and equivalent configuration fields describe the chipset-visible memory topology. They do not transfer guest-memory allocation, backing, or mapping ownership to Rigel.

The conceptual relationship remains:

~~~text
Bellatrix allocates/maps guest memory
            │
            ▼
Guest physical memory
            │
            ▼
Rigel is configured with the chipset-visible topology
            │
            ▼
Rigel determines how Agnus/chipset logic can access it
~~~

A configuration value describing Chip RAM capacity MUST NOT be interpreted as a request for `librigel` to allocate machine memory.

---

# 34. DMA Memory Boundary

Rigel requires access to guest memory for classic chipset DMA.

Examples include:

* bitplane fetches;
* Copper instruction fetches;
* Blitter reads and writes;
* audio DMA;
* sprite DMA;
* disk DMA where applicable.

Rigel accesses resolved guest physical memory through the host memory interface.

Conceptually:

~~~text
                  librigel
                      │
              chipset DMA semantics
                      │
                      ▼
            guest physical address
                      │
                      ▼
               rigel_host_ops
                      │
                      ▼
            Bellatrix memory model
                      │
                      ▼
                 Guest RAM
~~~

Rigel MUST NOT depend on Bellatrix internal memory structures.

---

# 35. DMA Address Semantics

DMA addresses produced by classic chipset registers are interpreted by Rigel according to the hardware model.

Rigel determines:

* pointer semantics;
* chipset-visible address masking;
* alignment behavior;
* chipset-visible address range;
* DMA ordering;
* Chip RAM accessibility.

The host is given only the resulting guest physical address required for the memory operation.

Conceptually:

~~~text
Chipset register state
          │
          ▼
Rigel address generation
          │
          ▼
guest physical address
          │
          ▼
host memory backend
~~~

Bellatrix MUST NOT duplicate chipset address-generation rules.

---

# 36. DMA Access API

The initial host memory interface MAY use callbacks such as:

~~~c
uint8_t
mem_read8(void *ctx, uint32_t guest_addr);

uint16_t
mem_read16(void *ctx, uint32_t guest_addr);

uint32_t
mem_read32(void *ctx, uint32_t guest_addr);

void
mem_write8(void *ctx, uint32_t guest_addr, uint8_t value);

void
mem_write16(void *ctx, uint32_t guest_addr, uint16_t value);

void
mem_write32(void *ctx, uint32_t guest_addr, uint32_t value);
~~~

The address passed to these operations represents a guest physical address, not:

* an Amiga register address;
* a chipset-relative offset;
* a host ARM pointer.

---

# 37. Direct Memory Windows

For performance, a future interface MAY expose validated direct guest-memory windows.

Conceptually:

~~~c
struct rigel_memory_window {
    uint32_t guest_base;
    size_t size;
    void *host_ptr;
};
~~~

This is optional.

Such an optimization MUST preserve exactly the same architectural semantics as callback-based memory access.

Direct mappings MUST NOT cause Rigel to depend on:

* Bellatrix allocator internals;
* Emu68 JIT structures;
* host page-table implementation;
* Raspberry Pi-specific memory layout.

---

# 38. DMA and MMIO Separation

Rigel DMA and M68K MMIO represent opposite directions across the integration boundary.

~~~text
M68K MMIO:

CPU
 │
 ▼
Bellatrix / Emu68
 │
 ▼
Rigel


Rigel DMA:

Rigel
 │
 ▼
Host memory interface
 │
 ▼
Guest RAM
~~~

These mechanisms MUST NOT be conflated.

In particular, Rigel DMA MUST NOT be implemented by recursively issuing M68K MMIO transactions through the Emu68 fault handler.

---

# 39. Memory Coherency

CPU and Rigel must observe a coherent guest-memory model.

After a CPU-visible memory write becomes architecturally committed, subsequent Rigel DMA must observe it according to the virtual timing model.

After a Rigel DMA write becomes architecturally visible, subsequent CPU access must observe it.

Optimizations involving:

* translated-code caches;
* direct memory mappings;
* host caches;
* write buffering;
* cross-core execution;

MUST preserve these semantics.

The exact synchronization mechanism is host-specific.

It belongs to Bellatrix/Emu68 integration, not to Rigel hardware semantics.

---

# 40. DMA-Written Executable Memory

Rigel DMA may write to guest memory containing M68K executable code.

In that case, Bellatrix/Emu68 remains responsible for JIT correctness.

Rigel performs only the guest-visible memory write.

Rigel MUST NOT know about:

* Emu68 translation blocks;
* JIT cache invalidation;
* translated AArch64 code;
* Emu68 internal page metadata;
* translation lookup structures.

The host memory backend must perform whatever invalidation or synchronization the execution engine requires.

---

# 41. Reset Model

The integration must distinguish at least:

~~~text
Bellatrix platform reset

Rigel cold reset

Rigel warm reset
~~~

The mapping between machine-level reset actions and these operations MUST be explicit.

A Bellatrix platform reset MAY reset both Bellatrix and Rigel.

A Rigel reset MUST NOT implicitly reset unrelated Raspberry Pi hardware.

---

# 42. Rigel Cold Reset

A Rigel cold reset returns the classic hardware model to its defined power-on state.

Conceptually:

~~~c
rigel_reset(rigel, RIGEL_RESET_COLD);
~~~

It may reset state associated with:

* DMA;
* Copper;
* Blitter;
* Paula;
* CIA;
* classic interrupt state;
* beam/timing state;
* internal chipset scheduling.

The exact hardware values and compatibility semantics belong to Rigel.

Bellatrix MUST NOT duplicate them.

---

# 43. Rigel Warm Reset

A warm reset represents the appropriate classic-machine reset semantics without reconstructing unrelated host platform state.

Conceptually:

~~~c
rigel_reset(rigel, RIGEL_RESET_WARM);
~~~

Rigel determines which classic hardware state is reset or preserved according to the selected compatibility model.

Bellatrix MUST NOT encode those chipset rules independently.

---

# 44. Initialization Sequence

When:

~~~text
CONFIG_RIGEL=n
~~~

Bellatrix initializes normally with no Rigel dependency.

When:

~~~text
CONFIG_RIGEL=y
~~~

the recommended sequence is:

~~~text
1. Bellatrix platform initialization
        │
2. Emu68 memory/address infrastructure
        │
3. Native platform initialization
        │
4. Construct Rigel host operations
        │
5. Create Rigel instance
        │
6. Establish guest-memory / Chip RAM mapping
        │
7. Register Rigel MMIO regions
        │
8. Reset Rigel
        │
9. Initialize IPL arbitration state
        │
10. Enable execution-progress synchronization
        │
11. Enter normal execution
~~~

Rigel initialization MUST NOT require native platform drivers to masquerade as classic Amiga devices.

---

# 45. Shutdown

If Bellatrix supports controlled shutdown or Rigel reinitialization, shutdown ordering must prevent callbacks after host resources have become invalid.

Conceptually:

~~~text
stop CPU execution
      │
stop progress delivery
      │
detach Rigel MMIO providers
      │
ensure no Rigel callback is active
      │
destroy Rigel
      │
release host resources
~~~

`rigel_destroy()` MUST NOT assume Bellatrix process semantics or operating-system services.

---

# 46. Configuration

Rigel-specific configuration SHOULD be passed explicitly when a Rigel instance is created.

Conceptually:

~~~c
struct rigel_config {
    enum rigel_chipset chipset;
    enum rigel_video_standard video_standard;
    uint32_t chip_ram_size;
    ...
};
~~~

Configuration describes the classic hardware environment being instantiated.

Fields describing memory capacity or topology describe Rigel's chipset-visible hardware view.

They do not change the memory-ownership rules defined by this specification.

Configuration MUST NOT describe unrelated Bellatrix native hardware.

For example:

* USB controller configuration;
* VC4 configuration;
* native SD configuration;
* BCM interrupt-controller configuration;

do not belong in `rigel_config`.

---

# 47. PAL and NTSC

Classic video standard selection belongs to Rigel configuration because it affects chipset timing.

Examples include:

~~~text
PAL
NTSC
~~~

Bellatrix may select the configuration.

Rigel owns the resulting:

* beam timing;
* scan timing;
* hardware event timing;
* chipset-visible behavior.

Bellatrix MUST NOT independently implement PAL or NTSC chipset timing.

---

# 48. Video Output Boundary

Rigel owns classic video-generation semantics.

Bellatrix owns native presentation hardware.

Conceptually:

~~~text
Classic chipset
      │
      ▼
    Rigel
      │
host-independent video representation
      │
      ▼
Bellatrix video adapter
      │
      ▼
native presentation
~~~

The integration specification deliberately does not require a particular output format.

Rigel MAY eventually expose an appropriate host-independent representation such as:

* completed frames;
* scanline-oriented output;
* incremental raster output;
* another stable abstraction.

The exact representation is a separate interface decision.

Normative rule:

> Rigel produces a host-independent representation of classic video output. Bellatrix adapts that representation to native presentation hardware.

Rigel MUST NOT depend directly on VC4.

Bellatrix MUST NOT implement Denise behavior.

---

# 49. Audio Output Boundary

The same ownership principle applies to audio.

Conceptually:

~~~text
Paula
  │
  ▼
Rigel
  │
host-independent audio representation
  │
  ▼
Bellatrix audio adapter
  │
  ▼
native audio presentation
~~~

The integration contract does not require a specific representation such as PCM buffers.

A future interface may expose:

* PCM samples;
* buffered audio blocks;
* timing-aware audio events;
* another host-independent representation.

Normative rule:

> Rigel produces a host-independent representation of classic audio output. Bellatrix adapts that representation to native audio hardware.

Rigel MUST NOT depend directly on:

* HDMI audio;
* PWM;
* USB audio;
* Raspberry Pi audio hardware.

---

# 50. Input Boundary

Native input devices belong to Bellatrix.

Classic hardware-visible input state belongs to Rigel when required by compatibility.

Conceptually:

~~~text
USB / Bluetooth / native input
            │
            ▼
     Bellatrix input adapter
            │
            ▼
 appropriate Rigel-facing interface
            │
       ┌────┼─────────────┐
       │    │             │
      CIA joystick    serial/input
          state          state
~~~

There is no requirement for all classic input to use one generic logical input structure.

Different classic interfaces MAY require distinct Rigel-facing APIs.

The important rule is:

> Rigel MUST NOT receive host-specific USB, Bluetooth, HID, or Raspberry Pi input objects.

Translation from native host input to the appropriate classic hardware abstraction belongs outside the host-independent Rigel core.

---

# 51. Logging

Rigel MAY expose diagnostic output through an optional host logging callback.

Conceptually:

~~~c
host_ops.log(...);
~~~

Logging MUST NOT be required for correctness.

Rigel MUST remain fully functional when no logging callback exists.

Diagnostic categories MAY include:

* Copper;
* Blitter;
* Paula;
* CIA;
* interrupts;
* DMA;
* video;
* timing.

Logging MUST NOT introduce timing semantics into the hardware model.

---

# 52. Determinism

Given identical defined:

* initial Rigel configuration;
* initial guest memory;
* MMIO transaction sequence;
* Rigel-facing input sequence;
* execution-progress sequence;

Rigel MUST produce identical defined:

* guest-memory effects;
* chipset state transitions;
* event ordering;
* interrupt state;
* video state;
* audio state.

Host wall-clock timing MUST NOT affect this result.

An explicitly documented future mode MAY intentionally introduce nondeterministic behavior.

Such a mode MUST be opt-in and MUST NOT redefine the deterministic semantics of the normal Rigel execution model.

Determinism is part of the integration contract because it enables reliable standalone harness testing and reproducible debugging.

---

# 53. Standalone Harness

The standalone Rigel harness MUST use the same public interface as Bellatrix.

Conceptually:

~~~text
                 ┌──────────────┐
                 │   Harness    │
                 └──────┬───────┘
                        │
                 public Rigel API
                        │
                 ┌──────▼───────┐
                 │   librigel   │
                 └──────┬───────┘
                        │
                   host callbacks
                        │
                 ┌──────▼───────┐
                 │ Harness RAM  │
                 │ logging      │
                 │ inspection   │
                 └──────────────┘
~~~

There MUST NOT be a separate simplified chipset implementation used only by the harness.

The purpose of the harness is to exercise production `librigel`.

---

# 54. Harness Capabilities

The standalone harness SHOULD eventually support:

* Rigel instance creation;
* initial guest-memory images;
* explicit Chip RAM configuration;
* direct MMIO transactions;
* deterministic execution advancement;
* synchronization-deadline inspection;
* IPL observation;
* memory inspection;
* video-state inspection;
* audio-state inspection;
* chipset diagnostics;
* event tracing;
* reproducible scripted tests.

The existing Bellatrix development workflow already emphasizes harness-generated logs and direct source-level investigation during debugging.

That workflow remains the preferred basis for validating the integration.

---

# 55. Integration Diagnostics

Bellatrix integration diagnostics SHOULD make it possible to distinguish at least:

~~~text
CPU execution
native IRQ
Rigel MMIO
Rigel timing
Rigel IRQ
Rigel DMA
video presentation
audio presentation
~~~

For example:

~~~text
[BELLATRIX:IRQ]

[RIGEL:MMIO]
[RIGEL:COPPER]
[RIGEL:DMA]
[RIGEL:IRQ]
~~~

The exact syntax is not normative.

The important property is that logs preserve architectural ownership and make it possible to identify which domain generated an observed event.

---

# 56. Threading and Core Placement

The public Rigel API MUST NOT encode a particular Raspberry Pi CPU-core topology.

Rigel may initially execute synchronously with the Emu68 execution path.

A future implementation MAY execute some work on another ARM core.

Neither choice should require redesigning the public host-independent API.

Therefore `librigel` MUST NOT assume:

* Core 0 ownership;
* Core 1 ownership;
* any fixed ARM core;
* a Bellatrix scheduler;
* WFE/SEV usage;
* Bellatrix lockless queues;
* Bellatrix-specific inter-core messaging.

Those are host implementation details.

---

# 57. Concurrency

Unless explicitly documented otherwise, a Rigel instance SHOULD initially be treated as single-thread-affine.

The host is responsible for serializing calls into that instance.

Combined with the non-reentrancy rule, this provides a deliberately simple initial execution model.

If future asynchronous:

* video;
* audio;
* logging;
* cross-core execution;

requires additional synchronization, that synchronization should be introduced at the host boundary or through explicitly specified Rigel interfaces.

Bellatrix synchronization primitives MUST NOT leak into the host-independent Rigel core.

---

# 58. Performance Optimizations

The integration boundary MAY later support optimizations such as:

* direct validated Chip RAM mappings;
* larger safe execution batches;
* improved deadline-driven execution;
* event coalescing;
* zero-copy video representations;
* zero-copy audio buffers;
* cross-core execution.

Such optimizations MUST NOT change architectural ownership.

Performance is not justification for moving:

* chipset timing;
* chipset register semantics;
* DMA semantics;
* interrupt semantics;

into Bellatrix.

---

# 59. Error Handling

Public Rigel operations that can fail SHOULD return explicit status information.

Initialization failures SHOULD distinguish conceptually between:

* invalid configuration;
* unsupported configuration;
* insufficient host services;
* guest-memory mapping failure;
* resource-allocation failure.

Runtime behavior representing valid classic hardware semantics SHOULD NOT be converted into a host integration error.

---

# 60. Versioning

The public `librigel` interface SHOULD expose an API version.

Conceptually:

~~~c
#define RIGEL_API_VERSION 1
~~~

or:

~~~c
uint32_t
rigel_api_version(void);
~~~

Bellatrix MUST depend only on documented public Rigel interfaces.

Internal Rigel structures are not ABI.

This allows Bellatrix and Rigel to evolve independently.

---

# 61. Build Boundary

The intended build relationship is:

~~~text
librigel
   │
   ├── public headers
   └── library
          ▲
          │
Bellatrix Rigel adapter
~~~

The Rigel core build MUST NOT require:

* Bellatrix headers;
* Bellatrix platform code;
* Emu68 internal headers;
* Raspberry Pi hardware headers;
* AROS headers.

If host-specific adapters are eventually added to the Rigel repository, they MUST remain separate from the host-independent core library.

---

# 62. Compile-Time Integration

Bellatrix controls Rigel integration using a build option such as:

~~~text
CONFIG_RIGEL=y
~~~

When disabled:

* no Rigel instance is created;
* no Rigel MMIO provider is registered;
* no Rigel progress path executes;
* no Rigel synchronization deadline participates in execution;
* no Rigel IPL source participates in arbitration;
* no Rigel DMA path exists;
* Bellatrix Core remains fully functional.

Conditional compilation SHOULD remain concentrated in the Rigel adapter and initialization boundary.

Rigel-specific conditionals SHOULD NOT spread through unrelated native platform components.

---

# 63. Architectural Non-Goals

The integration layer is not intended to:

* turn Raspberry Pi devices into Zorro boards;
* route native interrupts through Paula;
* make Rigel aware of BCM interrupt-controller internals;
* make Rigel aware of VC4 internals;
* make Rigel aware of Emu68 JIT internals;
* reproduce chipset timing in Bellatrix;
* reproduce Agnus DMA addressing in Bellatrix;
* expose Bellatrix internals to Rigel;
* require classic Amiga hardware compatibility for Bellatrix boot;
* make Rigel responsible for native hardware discovery;
* use host wall-clock time as chipset time;
* create a generic host-event escape mechanism.

---

# 64. Integration Invariants

The following rules are normative.

## Platform independence

Bellatrix Core MUST boot and operate without Rigel.

## Rigel independence

`librigel` MUST NOT depend on Bellatrix.

## Dependency direction

Bellatrix MAY depend on the public Rigel API.

Rigel MUST NOT depend on Bellatrix internals.

## MMIO ownership

Bellatrix MAY route classic MMIO transactions.

Bellatrix MUST NOT implement the semantics of Rigel-owned registers.

## Timing ownership

Bellatrix MUST provide virtual execution progress.

Rigel MUST interpret that progress as classic chipset time.

## Canonical chipset timeline

Rigel MUST own its internal authoritative chipset timeline.

Bellatrix MAY maintain synchronization accounting but MUST NOT independently maintain authoritative chipset time.

## Wall clock

Rigel chipset correctness MUST NOT depend on host wall-clock time.

## Synchronization deadlines

Rigel MAY define synchronization deadlines.

Bellatrix MUST give Rigel an opportunity to update its state when such a deadline is reached or crossed.

The host MUST NOT need to understand why the deadline exists.

## Deadline representation

The public ABI MUST explicitly define whether synchronization deadlines are absolute virtual timestamps or deltas relative to current Rigel time.

This distinction MUST NOT be inferred from call context.

## Overshoot

Rigel MUST tolerate deterministic execution overshoot across a synchronization deadline.

Overshoot support MUST NOT be treated as the preferred scheduling strategy.

## Interrupt ownership

Rigel MUST own classic `INTENA`/`INTREQ` state.

Bellatrix MUST own native interrupt state.

## Interrupt acceptance

CPU acceptance of an IPL MUST NOT automatically acknowledge, clear, or modify the originating Rigel interrupt source.

## Native interrupts

Native devices MUST NOT generate Paula interrupts.

## Rigel interrupts

Rigel MUST NOT inject chipset interrupt state through the native BCM interrupt-controller domain.

## IPL arbitration

Bellatrix MUST arbitrate CPU-visible IPL while preserving independent native and compatibility interrupt domains.

## IPL re-evaluation

Changes in either interrupt domain MUST cause the effective CPU-visible interrupt level to be re-evaluated as required by M68K interrupt semantics.

## Chip RAM ownership

Bellatrix MUST own guest physical memory allocation and mapping.

Rigel MUST own the chipset-visible interpretation of the guest-memory subset exposed as Chip RAM.

## Chip RAM configuration

Chip RAM size and equivalent configuration values MUST describe chipset-visible memory topology only.

They MUST NOT transfer allocation, backing, or mapping ownership to Rigel.

## Address spaces

Chipset-visible DMA addresses, guest physical addresses, MMIO addresses, and host pointers MUST remain conceptually distinct.

## DMA ownership

Rigel MUST own chipset DMA semantics and chipset address generation.

Bellatrix MUST provide guest-memory access without duplicating those semantics.

## JIT ownership

Rigel MUST NOT know about Emu68 translation or JIT cache internals.

## Host independence

Rigel MUST NOT depend on Raspberry Pi-specific services.

## Input independence

Rigel MUST NOT receive host-native USB, Bluetooth, HID, or Raspberry Pi input objects.

## Output independence

Rigel video and audio output MUST be represented independently of the native presentation hardware used by Bellatrix.

## Non-reentrancy

Unless explicitly documented otherwise, a callback invoked by Rigel MUST NOT re-enter the same Rigel instance.

## Determinism

Given identical defined inputs and configuration, normal Rigel operation MUST produce identical defined outputs and state transitions.

## Harness equivalence

The standalone harness MUST exercise the same production `librigel` interface used by Bellatrix.

## Optionality

Disabling Rigel MUST leave a functional Bellatrix Core platform.

---

# 65. Minimal Initial Integration

The first implementation does not need every possible optimization or presentation interface.

The minimum viable contract is:

~~~text
Lifecycle
    │
    ├── create
    ├── reset
    └── destroy

MMIO
    │
    ├── read
    └── write

Progress
    │
    ├── advance
    └── synchronization deadline

Interrupt
    │
    └── current IPL

Memory
    │
    ├── DMA read
    └── DMA write
~~~

Conceptually:

~~~c
struct rigel *rigel_create(...);

void rigel_reset(...);

void rigel_destroy(...);

uint16_t rigel_read16(...);

void rigel_write16(...);

void rigel_advance(...);

rigel_time_t rigel_next_deadline(...);

unsigned rigel_get_ipl(...);
~~~

plus the minimum host memory callbacks required for DMA.

These names are illustrative rather than ABI commitments.

Video, audio, input, diagnostics, optimized memory windows, and asynchronous notifications MAY be layered onto this boundary later.

---

# 66. Recommended Implementation Order

The first implementation SHOULD proceed in dependency order:

~~~text
1. Extract/build librigel independently
          │
2. Define opaque Rigel instance
          │
3. Define minimal host memory callbacks
          │
4. Establish Chip RAM ownership/mapping contract
          │
5. Implement standalone harness
          │
6. Define MMIO API
          │
7. Attach Bellatrix MMIO adapter
          │
8. Select canonical execution-progress unit
          │
9. Define authoritative Rigel timeline semantics
          │
10. Define absolute-vs-relative deadline ABI semantics
          │
11. Implement advance/deadline contract
          │
12. Expose Rigel IPL
          │
13. Implement Bellatrix IPL arbitration/re-evaluation
          │
14. Validate interrupt acceptance semantics
          │
15. Validate DMA coherency and JIT interaction
          │
16. Add host-independent video/audio boundaries
          │
17. Add native input adaptation
~~~

The standalone harness appears deliberately early.

This makes Rigel independence and determinism demonstrated implementation properties rather than merely architectural statements.

---

# 67. Validation Criteria

The integration SHOULD NOT be considered complete merely because AROS boots with Rigel enabled.

At minimum, validation should demonstrate the following.

## Core independence

~~~text
CONFIG_RIGEL=n
~~~

boots and operates normally.

## Rigel independence

The standalone Rigel harness builds and operates without Bellatrix.

## Build isolation

`librigel` builds without:

* Bellatrix;
* Raspberry Pi-specific headers;
* Emu68 internal headers;
* AROS headers.

## MMIO isolation

Classic register semantics exist only inside Rigel.

## Native interrupt isolation

Native hardware interrupts operate without using `INTENA` or `INTREQ`.

## Rigel interrupt isolation

Rigel chipset interrupts operate without entering the BCM interrupt-controller domain.

## Interrupt acceptance

Accepting an M68K interrupt exception does not automatically clear the originating Rigel source.

## IPL re-evaluation

When a higher-priority source disappears, a still-pending lower-priority source remains correctly visible to the CPU.

## Timing ownership

Changing host pacing does not change deterministic Rigel behavior for an identical execution-progress sequence.

## Timeline ownership

Bellatrix synchronization counters can be changed or reorganized without creating an independent source of authoritative chipset time.

## Deadline correctness

Crossing a Rigel synchronization deadline results in deterministic state updates.

## Deadline representation

The public ABI unambiguously documents whether deadlines are absolute or relative.

## Overshoot correctness

A controlled amount of execution overshoot produces the same logical result as equivalent finer-grained advancement.

## Chip RAM ownership

Bellatrix remains responsible for physical guest-memory allocation while Rigel controls chipset-visible access semantics.

## Chip RAM configuration

Changing `chip_ram_size` or equivalent topology configuration does not cause `librigel` to assume ownership of guest-memory allocation or mapping.

## DMA correctness

Rigel DMA and M68K CPU accesses observe a coherent guest-memory model.

## JIT correctness

DMA writes affecting executable guest memory are correctly reflected by Emu68 without exposing JIT internals to Rigel.

## Reset correctness

Rigel can be reset without reconstructing unrelated native platform state.

## Determinism

Repeated harness runs with identical defined inputs produce identical defined Rigel outputs and state transitions.

## Harness equivalence

Behavior observed through the harness uses the same production Rigel implementation and API used by Bellatrix.

---

# 68. Review Criteria

Future patches affecting Bellatrix/Rigel integration SHOULD be reviewed against the following questions:

1. Does this code belong to the native platform or to classic hardware compatibility?
2. Is Bellatrix learning internal Rigel semantics?
3. Is Rigel acquiring a dependency on Bellatrix?
4. Is native hardware being represented unnecessarily through Amiga hardware mechanisms?
5. Is chipset timing being computed outside Rigel?
6. Is Bellatrix creating a second authoritative chipset timeline?
7. Is a host wall clock being allowed to define chipset state?
8. Is native interrupt state entering `INTENA` or `INTREQ`?
9. Is Rigel interrupt state entering the BCM interrupt domain?
10. Is CPU exception acceptance being confused with hardware-source acknowledgement?
11. Does the IPL implementation correctly re-evaluate remaining sources after a level changes?
12. Does Bellatrix need to understand why a Rigel synchronization deadline exists?
13. Is the public ABI explicit about absolute versus relative deadline representation?
14. Is deadline overshoot being tolerated for correctness without becoming the default batching strategy?
15. Are chipset-visible addresses being confused with guest physical addresses?
16. Are guest physical addresses being confused with host pointers?
17. Is Chip RAM allocation ownership remaining in Bellatrix?
18. Is `chip_ram_size` being incorrectly treated as an allocation request?
19. Is chipset address-generation behavior remaining in Rigel?
20. Is chipset DMA behavior being duplicated in the host?
21. Is Rigel becoming aware of Emu68 JIT internals?
22. Is a generic callback being introduced where a narrower contract would suffice?
23. Can a Rigel callback accidentally re-enter the same Rigel instance?
24. Does video/audio output remain independent of native presentation hardware?
25. Does Rigel remain unaware of USB, Bluetooth, and native HID objects?
26. Is deterministic behavior preserved for identical defined inputs?
27. Does the change preserve standalone Rigel testing?
28. Does `CONFIG_RIGEL=n` remain a first-class supported configuration?

If a patch cannot answer these questions cleanly, the integration boundary should be reconsidered before acceptance.

---

# 69. Final Integration Model

The complete relationship is:

~~~text
                            AROS/m68k
                                │
                         m68k-emu68
                                │
                              Emu68
                                │
                   Bellatrix Native Platform
                                │
             ┌──────────────────┴──────────────────┐
             │                                     │
       Native Hardware                      Rigel Adapter
             │                                     │
     BCM / VC4 / SD / USB            ┌─────────────┼─────────────┐
             │                       │             │             │
             │                      MMIO        Progress         IPL
             │                       │             │             │
             │                       └─────────────┼─────────────┘
             │                                     │
             │                                 librigel
             │                                     │
             │                        ┌────────────┼────────────┐
             │                        │            │            │
             │                      Agnus        Denise       Paula
             │                        │                         │
             │                  Copper/Blitter              CIAA/CIAB
             │                        │                         │
             │                        └──────── DMA ────────────┘
             │                                     │
             │                          chipset-visible address
             │                                     │
             │                            Rigel translation
             │                                     │
             │                            guest physical address
             │                                     │
             │                              Host Memory API
             │                                     │
             └──────────────────── Guest Memory ───┘
~~~

CPU interrupt delivery remains:

~~~text
Native hardware
      │
      ▼
native_ipl ─────────────┐
                        │
                        ▼
                   IPL arbitration
                   and re-evaluation
                        │
                        ▼
                      M68K
                        ▲
                        │
rigel_ipl ──────────────┘
      ▲
      │
   librigel
~~~

Neither interrupt domain owns or acknowledges the other.

The memory relationship is:

~~~text
Bellatrix allocation
        │
        ▼
Guest physical memory
        ▲
        │
Host memory backend
        ▲
        │
resolved guest address
        ▲
        │
Rigel address semantics
        ▲
        │
Chipset-visible address
~~~

The timing relationship is:

~~~text
Emu68 execution
      │
      ▼
Bellatrix progress accounting
      │
      ▼
virtual execution progress
      │
      ▼
Rigel authoritative chipset timeline
      │
      ├── chipset state
      └── next synchronization deadline
                    │
                    ▼
                Bellatrix
~~~

The complete cross-boundary relationship is:

~~~text
                 Bellatrix                  Rigel
                     │                        │
CPU execution        ├──── progress ─────────►│
                     │                        │
MMIO                 ├──── transaction ─────►│
                     │                        │
Guest memory         │◄──── DMA access ──────┤
                     │                        │
Interrupt            │◄──── rigel_ipl ───────┤
                     │                        │
Scheduling           │◄──── deadline ────────┤
                     │                        │
Input                ├──── classic input ────►│
                     │                        │
Presentation         │◄──── video/audio ─────┤
~~~

None of these relationships requires either side to know the implementation of the other.

---

# 70. Integration Definition

The Bellatrix/Rigel integration can be summarized as follows:

> Bellatrix hosts Rigel; it does not implement Rigel.

> Rigel models classic Amiga hardware; it does not implement the Bellatrix platform.

Bellatrix provides:

* M68K execution;
* address dispatch;
* guest physical memory;
* memory allocation and mapping;
* native hardware;
* native interrupts;
* execution-progress accounting;
* native presentation;
* host input.

Rigel provides:

* classic chipset semantics;
* classic MMIO;
* the authoritative chipset timeline;
* chipset timing;
* synchronization deadlines;
* chipset-visible memory semantics;
* chipset DMA;
* classic interrupt state;
* compatibility-domain IPL;
* classic video generation;
* classic audio generation;
* classic hardware-facing input state.

The Bellatrix adapter connects these domains without merging them.

This boundary is the implementation contract that preserves the architecture defined by `Bellatrix.md`.

---

# 71. Specification Hierarchy

The intended documentation hierarchy is:

~~~text
Bellatrix.md
    │
    │ defines
    ▼
architecture
responsibilities
ownership
boundaries
invariants
    │
    ▼
Bellatrix-Rigel-Integration.md
    │
    │ defines
    ▼
cross-boundary behavioral contract
MMIO
time
timeline ownership
deadlines
IPL
DMA
memory
lifecycle
build
testing
    │
    ▼
include/rigel/rigel.h
    │
    │ materializes
    ▼
public C ABI
~~~

`rigel.h` SHOULD materialize the integration specification rather than create new architectural concepts.

If implementation work reveals the need for a new cross-boundary abstraction, that abstraction SHOULD first be incorporated into this specification and only then exposed through the public C API.

The public header must therefore be a consequence of the specification rather than the place where the architecture is discovered during implementation.

This keeps implementation decisions subordinate to the architecture and prevents the concrete ABI from accidentally redefining the integration boundary.
