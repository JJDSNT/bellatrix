Bellatrix / Rigel Integration Specification

Host Interface, MMIO, Timing, Interrupts, DMA, and Lifecycle

Status: Proposed Integration Specification

This document defines the integration contract between Bellatrix and librigel.

The architectural responsibilities and ownership rules are defined by Bellatrix.md. This document does not redefine those decisions.

Its purpose is to specify how Bellatrix, acting as a Rigel host, attaches the optional classic Amiga hardware compatibility layer to the native M68K platform.

The fundamental dependency direction is:

Bellatrix
    │
    │ public Rigel API
    ▼
 librigel
librigel
    │
    │ host operations
    ▼
Host-provided services

Bellatrix may depend on the public Rigel interface.

librigel MUST NOT depend on Bellatrix internals.

⸻

1. Integration Goals

The Bellatrix/Rigel integration must provide the minimum mechanisms required for Rigel to operate as a complete classic Amiga hardware compatibility component.

The integration boundary consists of:

* lifecycle control;
* MMIO dispatch;
* execution progress delivery;
* interrupt-level delivery;
* memory access for chipset DMA;
* reset propagation;
* optional diagnostic services.

The boundary must not expose Rigel internals to Bellatrix.

Bellatrix must not directly operate:

* Agnus;
* Denise;
* Paula;
* CIAA;
* CIAB;
* Copper;
* Blitter;
* beam state;
* chipset DMA scheduling;
* INTENA;
* INTREQ.

Those remain exclusively owned by Rigel.

⸻

2. Integration Model

Conceptually:

                         Bellatrix
                             │
                    Bellatrix Rigel Adapter
                             │
              ┌──────────────┼──────────────┐
              │              │              │
             MMIO          Progress         IPL
              │              │              │
              └──────────────┼──────────────┘
                             │
                         librigel
                             │
             ┌───────────────┼───────────────┐
             │               │               │
           Agnus           Denise          Paula
             │                               │
          Copper                         CIAA/CIAB
          Blitter

The Bellatrix adapter is responsible only for translating between the Bellatrix/Emu68 platform boundary and the public Rigel API.

It MUST NOT contain chipset behavior.

⸻

3. Component Boundary

The recommended source-level separation is:

Bellatrix
│
├── platform/
│
├── emu68/
│
├── ...
│
└── rigel_adapter.c
          │
          ▼
      librigel

The exact Bellatrix directory layout is not normative.

The architectural requirement is that Rigel integration remain isolated behind a small adapter.

A useful test is:

Removing the Bellatrix Rigel adapter and disabling CONFIG_RIGEL must remove the compatibility layer without requiring structural changes elsewhere in the native platform.

⸻

4. Public Rigel API

The public API should expose operations on an opaque Rigel instance.

Conceptually:

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
rigel_reset(struct rigel *rigel, enum rigel_reset_type type);

The exact names are implementation details.

The following properties are normative:

* Rigel state MUST be encapsulated.
* Bellatrix MUST NOT require access to internal Rigel structures.
* Host-specific state MUST be supplied through an opaque host context.
* Rigel MUST support independent instantiation by a test harness.

Global Bellatrix-specific state MUST NOT be required by librigel.

⸻

5. Host Operations

Rigel may require services from its host.

These services should be represented through an explicit callback table.

Conceptually:

struct rigel_host_ops {
    uint8_t  (*mem_read8)(void *ctx, uint32_t addr);
    uint16_t (*mem_read16)(void *ctx, uint32_t addr);
    uint32_t (*mem_read32)(void *ctx, uint32_t addr);
    void (*mem_write8)(void *ctx, uint32_t addr, uint8_t value);
    void (*mem_write16)(void *ctx, uint32_t addr, uint16_t value);
    void (*mem_write32)(void *ctx, uint32_t addr, uint32_t value);
    void (*signal_event)(void *ctx, uint32_t event);
    void (*log)(void *ctx, int level, const char *message);
};

This structure is illustrative.

Only callbacks that are actually required should exist.

In particular, the interface SHOULD NOT become a generic escape mechanism through which Rigel calls arbitrary Bellatrix functionality.

⸻

6. Host Context

The host context is opaque to Rigel.

Bellatrix may use it to identify the owning platform instance:

void *host_context;

Rigel MUST:

* store the value without interpreting it;
* pass it unchanged to host callbacks;
* make no assumptions about its contents.

This allows the same librigel binary to operate with Bellatrix, a standalone harness, or another future host.

⸻

7. MMIO Registration

When Rigel is enabled, Bellatrix registers the classic hardware regions handled by Rigel with the Emu68 address/fault dispatcher.

Conceptually:

Emu68 address dispatcher
        │
        ├── native mappings
        │
        ├── Rigel custom region
        │
        ├── Rigel CIAA region
        │
        └── Rigel CIAB region

Bellatrix owns registration of the address regions.

Rigel owns the semantics of accesses within those regions.

This distinction is normative.

⸻

8. MMIO Dispatch

An intercepted classic hardware access follows this path:

M68K instruction
      │
      ▼
Emu68 MMU / fault handling
      │
      ▼
Bellatrix Rigel adapter
      │
      ▼
librigel
      │
      ▼
Chipset register implementation

The Bellatrix adapter may translate the host’s representation of an MMIO transaction into the public Rigel API.

It MUST NOT interpret the target register.

For example, Bellatrix may determine:

address = 0xDFF096
width   = 16
type    = WRITE
value   = 0x8200

but the meaning of that write belongs entirely to Rigel.

Bellatrix MUST NOT contain logic equivalent to:

if (addr == DMACON)
    update_dma_state(...);

Such logic belongs in librigel.

⸻

9. MMIO Access API

The initial interface may conceptually provide:

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

The exact supported widths may differ by region.

Unsupported or historically unusual accesses must have explicitly defined behavior rather than relying on host behavior accidentally.

⸻

10. Address Representation

The address supplied to Rigel should use a single canonical representation.

The preferred model is the M68K-visible physical address:

0x00DFFxxx
0x00BFxxxx
0x00BFxxxx
...

rather than offsets whose interpretation depends on the caller.

This keeps the API self-describing and allows the same dispatch interface to support multiple compatibility regions.

If offsets are used instead, that decision must be globally consistent and documented by the API.

⸻

11. Endianness

The M68K-visible hardware model is big-endian.

Host hardware may use a different native byte order.

The integration boundary MUST define where byte-order conversion occurs.

The recommended rule is:

Values crossing the public Rigel MMIO API represent M68K-visible logical values.

Therefore:

rigel_write16(rigel, 0xDFF096, 0x8200);

means the M68K guest wrote the 16-bit value 0x8200, independently of ARM host byte order.

Host-native byte representation MUST NOT leak into Rigel register semantics.

⸻

12. Unmapped Accesses

Rigel should only receive accesses to regions registered as belonging to Rigel.

An address outside those regions remains the responsibility of the Emu68/Bellatrix address dispatcher.

Conceptually:

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

Rigel MUST NOT become the generic fallback handler for arbitrary M68K addresses.

⸻

13. Autoconfig Provider

The historical Autoconfig region is not intrinsically owned by Rigel merely because Rigel is enabled.

If classic Autoconfig is required, it must be represented as an explicit compatibility provider.

Conceptually:

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

This provider may eventually be implemented by Rigel or another compatibility component, but the decision is independent from the basic chipset integration.

CONFIG_RIGEL=y MUST NOT automatically imply a virtual Zorro bus unless explicitly configured.

⸻

14. Execution Progress Contract

Bellatrix supplies execution progress to Rigel.

Rigel interprets that progress according to classic Amiga hardware timing.

Conceptually:

M68K execution
      │
      ▼
Execution progress
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

Bellatrix MUST NOT convert execution progress into chipset-specific units.

⸻

15. Progress Unit

The public interface must define exactly one canonical progress unit.

Possible implementations include:

rigel_advance_cycles(...);

or:

rigel_advance_ns(...);

The final unit must satisfy:

* deterministic conversion;
* sufficient precision for chipset timing;
* efficient use in the execution loop;
* stable semantics across hosts;
* no dependency on host wall-clock time.

The selected unit MUST represent virtual execution progress, not elapsed real-world time.

⸻

16. No Wall-Clock-Driven Chipset

Rigel timing MUST NOT be driven directly from:

* ARM generic timer wall time;
* host scheduler latency;
* USB timing;
* display refresh timing;
* gettimeofday()-style sources;
* other real-time host clocks.

Host clocks may be used for pacing the overall emulator/platform externally.

They MUST NOT define chipset state.

The distinction is:

Virtual execution progress ───► Rigel correctness
Host wall clock ──────────────► optional pacing

This is required for deterministic behavior.

⸻

17. Progress Delivery

Bellatrix may accumulate execution progress before advancing Rigel.

Conceptually:

CPU executes
     │
     ▼
progress accumulator
     │
     ├── below threshold → continue
     │
     └── threshold reached
                │
                ▼
          rigel_advance(...)

The batching policy belongs to the Bellatrix adapter.

However, batching MUST preserve externally observable chipset behavior.

In particular, Bellatrix MUST NOT advance beyond a Rigel event that should become visible to the CPU before the end of the batch.

⸻

18. Next-Event Interface

Rigel should provide enough information for the host to avoid unnecessary fine-grained stepping.

Conceptually:

rigel_time_t
rigel_next_event(const struct rigel *rigel);

This represents the amount of execution progress until Rigel may next produce an externally observable state change.

Possible events include:

* interrupt-level changes;
* DMA-visible memory effects;
* timing-sensitive MMIO state changes;
* other chipset events requiring host synchronization.

The exact event model must be defined by the implementation.

The architectural objective is to allow Bellatrix to execute efficiently without requiring Rigel to be stepped after every M68K instruction.

⸻

19. Deadline-Based Execution

The preferred execution relationship is deadline based.

Conceptually:

Rigel
 │
 │ next externally relevant event
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

Rigel therefore determines when its next externally relevant event occurs.

Bellatrix determines how CPU execution reaches that point.

This preserves the principle:

The CPU reports progress. The chipset owns chipset time.

⸻

20. Overshoot

The integration must explicitly account for execution progress that passes a Rigel deadline.

For example:

Rigel deadline: 100 units
CPU reports:    112 units

Rigel must be capable of advancing through the event at 100 and continuing through the remaining 12 units deterministically.

The API MUST NOT require Bellatrix to predict exact instruction boundaries matching every chipset event.

The implementation should minimize overshoot where CPU-visible timing requires it, but correctness must not depend on zero overshoot.

⸻

21. Rigel Interrupt State

Rigel owns classic Amiga interrupt state.

This includes:

* INTENA;
* INTREQ;
* interrupt source state;
* classic priority resolution.

Bellatrix MUST NOT independently maintain a second authoritative copy of this state.

The CPU-visible output of this subsystem is the current Rigel IPL.

Conceptually:

Chipset events
      │
      ▼
    INTREQ
      │
      ▼
INTREQ & INTENA
      │
      ▼
priority resolution
      │
      ▼
   Rigel IPL

⸻

22. IPL Interface

Rigel should expose its current CPU-visible interrupt level.

Conceptually:

unsigned
rigel_get_ipl(const struct rigel *rigel);

The value represents the M68K interrupt priority level currently asserted by the classic Amiga hardware domain.

Bellatrix does not need to know which internal Rigel source produced that level.

⸻

23. IPL Change Notification

Polling rigel_get_ipl() after every CPU operation is undesirable.

Rigel may therefore notify the host when its externally visible IPL changes.

Conceptually:

void (*ipl_changed)(void *ctx, unsigned new_ipl);

Alternatively, IPL changes may be surfaced through a generic event mechanism.

The implementation choice is not normative.

The required property is:

Bellatrix must be able to observe a Rigel IPL transition before CPU execution incorrectly proceeds past a point where that transition should have become visible.

⸻

24. Native and Rigel IPL Arbitration

Bellatrix may simultaneously have:

native_ipl
rigel_ipl

These belong to independent interrupt domains.

The Bellatrix adapter is responsible for producing the effective CPU-visible IPL.

Conceptually:

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

For the initial implementation, the effective level should be the highest currently asserted priority:

effective_ipl = max(native_ipl, rigel_ipl);

This preserves M68K interrupt priority semantics while keeping source ownership independent.

The arbitration layer MUST NOT merge the underlying interrupt state.

⸻

25. Interrupt Acknowledge

IPL delivery and interrupt acknowledgement are separate operations.

When the M68K accepts an interrupt, the integration must preserve ownership of the corresponding interrupt domain.

A native interrupt acknowledgement remains within the native platform interrupt path.

A Rigel interrupt acknowledgement remains governed by classic Amiga hardware semantics.

Bellatrix MUST NOT clear Rigel INTREQ merely because an IPL was accepted by the CPU.

Likewise, Rigel MUST NOT acknowledge or clear native BCM interrupt state.

⸻

26. DMA Memory Boundary

Rigel requires access to guest memory for classic chipset DMA.

Examples include:

* bitplane fetches;
* Copper instruction fetches;
* Blitter reads and writes;
* audio DMA;
* sprite DMA;
* disk DMA where applicable.

Rigel MUST access this memory through an explicitly defined memory boundary.

Conceptually:

                  librigel
                      │
                    DMA
                      │
                      ▼
               rigel_host_ops
                      │
                      ▼
            Bellatrix memory model
                      │
                      ▼
                 Guest RAM

Rigel MUST NOT depend on Bellatrix internal memory implementation.

⸻

27. DMA Address Semantics

DMA addresses presented by classic chipset registers are interpreted according to Rigel’s hardware model.

Rigel determines:

* pointer semantics;
* address masking;
* alignment;
* chipset-visible address range;
* DMA ordering.

The host determines only how a resolved guest physical address is read or written.

Therefore:

Chipset pointer semantics
          │
          ▼
        Rigel
          │
resolved guest address
          │
          ▼
      Host memory API

Bellatrix MUST NOT reproduce Agnus address-generation rules.

⸻

28. DMA Access API

The initial host memory interface may use operations such as:

uint8_t  mem_read8(void *ctx, uint32_t addr);
uint16_t mem_read16(void *ctx, uint32_t addr);
uint32_t mem_read32(void *ctx, uint32_t addr);
void mem_write8(void *ctx, uint32_t addr, uint8_t value);
void mem_write16(void *ctx, uint32_t addr, uint16_t value);
void mem_write32(void *ctx, uint32_t addr, uint32_t value);

For performance, a future API may additionally expose validated direct memory windows.

For example:

struct rigel_memory_window {
    uint32_t guest_base;
    size_t size;
    void *host_ptr;
};

Such optimization MUST preserve the same architectural memory semantics as callback-based access.

⸻

29. DMA and MMIO Separation

Rigel DMA access and M68K MMIO access are different directions across the integration boundary.

M68K MMIO:
CPU
 │
 ▼
Bellatrix
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

These mechanisms MUST NOT be conflated.

In particular, DMA must not be implemented by recursively issuing M68K MMIO transactions through the Emu68 fault handler.

⸻

30. Memory Coherency

CPU and Rigel must observe a coherent guest memory model.

After a CPU-visible write becomes architecturally committed, subsequent Rigel DMA must observe it according to the timing model.

After a Rigel DMA write becomes architecturally visible, subsequent CPU access must observe it.

Any optimization involving:

* translated-code caches;
* direct memory mappings;
* host caches;
* buffered writes;

must preserve these semantics.

The exact synchronization mechanism is host-specific and belongs to the Bellatrix adapter/Emu68 integration.

⸻

31. Self-Modifying and DMA-Written Code

If Rigel DMA can modify memory containing executable M68K code, Bellatrix/Emu68 remains responsible for maintaining JIT correctness.

Rigel only performs the guest-visible memory write.

It MUST NOT know about:

* Emu68 translation blocks;
* JIT cache invalidation;
* translated ARM code;
* Emu68 internal page metadata.

The host memory implementation must perform any required invalidation.

⸻

32. Reset Model

The integration must distinguish at least:

Bellatrix platform reset
Rigel cold reset
Rigel warm reset

The exact mapping from Amiga reset behavior to these operations must be explicit.

A platform reset may reset both Bellatrix and Rigel.

A Rigel reset MUST NOT implicitly reset unrelated Raspberry Pi hardware.

⸻

33. Cold Reset

A Rigel cold reset returns the classic hardware model to its power-on state.

Conceptually:

rigel_reset(rigel, RIGEL_RESET_COLD);

It should reset all relevant chipset state including:

* DMA state;
* Copper;
* Blitter;
* Paula;
* CIA state;
* interrupt state;
* beam/timing state;
* internal event scheduling.

The exact power-on values belong to Rigel.

⸻

34. Warm Reset

A warm reset represents the appropriate classic machine reset semantics without reconstructing the host platform.

Conceptually:

rigel_reset(rigel, RIGEL_RESET_WARM);

Rigel determines which classic hardware state survives or resets according to the selected compatibility model.

Bellatrix MUST NOT duplicate those rules.

⸻

35. Initialization Sequence

When CONFIG_RIGEL=n, Bellatrix initializes normally with no Rigel-specific dependency.

When CONFIG_RIGEL=y, the recommended sequence is:

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
6. Register Rigel MMIO regions
        │
7. Reset Rigel
        │
8. Initialize IPL arbitration
        │
9. Enable execution progress delivery
        │
10. Enter normal execution

Rigel creation MUST NOT require native platform drivers to masquerade as Amiga devices.

⸻

36. Shutdown

If Bellatrix supports a controlled shutdown or reinitialization path, the sequence should prevent Rigel callbacks after host resources have become invalid.

Conceptually:

stop CPU execution
      │
stop Rigel progress
      │
detach MMIO regions
      │
destroy Rigel
      │
release host resources

rigel_destroy() MUST NOT assume Bellatrix process semantics or operating-system services.

⸻

37. Configuration

Rigel-specific configuration should be passed explicitly at instance creation.

Conceptually:

struct rigel_config {
    enum rigel_chipset chipset;
    enum rigel_video_standard video_standard;
    uint32_t chip_ram_size;
    ...
};

Configuration must describe the compatibility hardware being instantiated.

It MUST NOT describe unrelated Bellatrix platform hardware.

For example, Raspberry Pi USB configuration does not belong in rigel_config.

⸻

38. PAL and NTSC

Video standard selection belongs to Rigel configuration because it affects classic chipset timing.

For example:

PAL
NTSC

Bellatrix may choose the configuration value.

Rigel owns the resulting:

* scan timing;
* beam behavior;
* chipset event timing;
* related hardware semantics.

Bellatrix MUST NOT independently implement PAL/NTSC chipset timing.

⸻

39. Video Output Boundary

Rigel owns classic video generation.

Bellatrix owns native display hardware.

Therefore the integration requires a presentation boundary between them.

Conceptually:

Classic chipset
      │
      ▼
    Rigel
      │
logical video output
      │
      ▼
Bellatrix video adapter
      │
      ▼
VC4 / framebuffer / native display

Rigel MUST NOT depend directly on VC4.

Bellatrix MUST NOT implement Denise behavior.

The exact representation of the video output surface is a separate implementation contract and may evolve independently from chipset semantics.

⸻

40. Audio Output Boundary

The same principle applies to audio.

Paula
  │
  ▼
Rigel
  │
audio samples/events
  │
  ▼
Bellatrix audio adapter
  │
  ▼
native audio hardware

Rigel owns Paula audio semantics.

Bellatrix owns native audio transport and presentation.

Rigel MUST NOT depend directly on HDMI audio, PWM, USB audio, or Raspberry Pi audio hardware.

⸻

41. Input Boundary

Native input hardware belongs to Bellatrix.

Classic input hardware state belongs to Rigel when compatibility requires it.

Conceptually:

USB / Bluetooth HID
        │
        ▼
    Bellatrix
        │
input translation
        │
        ▼
      Rigel
        │
CIA / classic input state

The translation layer must remain outside Rigel’s host-independent chipset core where the source is host-specific.

Rigel should receive logical classic hardware input state rather than Raspberry Pi USB or Bluetooth events.

⸻

42. Logging

Rigel may expose diagnostic output through the host interface.

For example:

host_ops.log(...)

Logging MUST NOT be required for correctness.

Rigel MUST remain usable when no logging callback is supplied.

Log categories should allow selective inspection of areas such as:

* Copper;
* Blitter;
* Paula;
* CIA;
* interrupts;
* DMA;
* video;
* timing.

⸻

43. Determinism

Given identical:

* initial Rigel configuration;
* initial guest memory;
* MMIO transaction sequence;
* input sequence;
* execution-progress sequence;

Rigel SHOULD produce identical:

* memory effects;
* chipset state;
* event ordering;
* interrupt state;
* video state;
* audio state.

Host wall-clock timing MUST NOT affect this result.

This property is required for reliable harness testing.

⸻

44. Standalone Harness

The standalone harness should use exactly the same public interface as Bellatrix.

Conceptually:

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

There MUST NOT be a separate simplified chipset implementation used only by the harness.

The harness exists to exercise the production librigel.

⸻

45. Harness Capabilities

The harness should eventually support:

* creation of a Rigel instance;
* loading an initial memory image;
* direct MMIO transactions;
* deterministic execution advancement;
* interrupt observation;
* memory inspection;
* video-state inspection;
* chipset-state diagnostics;
* event tracing;
* reproducible test scripts.

This preserves the existing project philosophy of using real execution logs and source-level validation as the primary debugging mechanism.

⸻

46. Integration Diagnostics

Bellatrix integration debugging should make it possible to distinguish at least:

CPU execution
native IRQ
Rigel MMIO
Rigel timing
Rigel IRQ
DMA
video
audio

Diagnostic logging should preserve the ownership boundary.

For example:

[BELLATRIX:IRQ]
[RIGEL:MMIO]
[RIGEL:COPPER]
[RIGEL:DMA]
[RIGEL:IRQ]

The exact logging syntax is not normative.

The important property is that logs make it possible to determine which architectural domain produced an event.

⸻

47. Threading and Core Placement

The public Rigel API MUST NOT encode a particular Raspberry Pi core topology.

Rigel may initially execute synchronously with the Emu68 execution path.

A future implementation may execute parts of Rigel on another ARM core.

Neither choice should require redesigning the public API.

Therefore librigel MUST NOT assume:

* Core 0 ownership;
* Core 1 ownership;
* a specific Bellatrix scheduler;
* a particular WFE/SEV protocol;
* Bellatrix-specific lockless queues.

Those are host implementation decisions.

⸻

48. Concurrency

Unless explicitly documented otherwise, a Rigel instance may initially be treated as single-thread-affine.

That means the host is responsible for serializing calls into a given instance.

This is preferable to embedding Bellatrix-specific synchronization primitives inside librigel.

If asynchronous presentation, audio, or other consumers require cross-core communication, that synchronization belongs at the host boundary.

⸻

49. Performance Optimizations

The integration boundary may later support optimizations such as:

* direct validated Chip RAM mappings;
* batched MMIO;
* deadline-based execution;
* event coalescing;
* zero-copy video surfaces;
* zero-copy audio buffers;
* cross-core execution.

Such optimizations MUST NOT alter the architectural ownership rules.

Performance is not justification for moving chipset semantics into Bellatrix.

⸻

50. Error Handling

Public Rigel operations that may fail should return explicit status information.

Initialization failures should distinguish at least conceptually between:

* invalid configuration;
* unsupported configuration;
* insufficient host services;
* resource allocation failure.

Runtime hardware behavior that represents normal classic-machine semantics should not be treated as a host integration error.

⸻

51. Versioning

The public librigel interface should expose an API version.

Conceptually:

#define RIGEL_API_VERSION 1

or:

uint32_t rigel_api_version(void);

Bellatrix should depend only on the documented public API.

Internal Rigel structures are not ABI.

This allows Rigel and Bellatrix to evolve independently.

⸻

52. Build Boundary

The intended build relationship is:

librigel
   │
   ├── public headers
   └── library
          ▲
          │
Bellatrix Rigel adapter

The Rigel core build MUST NOT require:

* Bellatrix headers;
* Bellatrix platform code;
* Emu68 internal headers;
* Raspberry Pi hardware headers;
* AROS headers;

unless a future explicitly separated adapter requires them outside the core library.

The public library should remain host-independent.

⸻

53. Compile-Time Integration

Bellatrix controls integration using a build option such as:

CONFIG_RIGEL=y

When disabled:

* no Rigel instance is created;
* no Rigel MMIO providers are registered;
* no Rigel timing path executes;
* no Rigel IPL source participates in arbitration;
* the native Bellatrix architecture remains unchanged.

Conditional compilation should preferably remain concentrated in the Rigel adapter and initialization boundary rather than spreading throughout the platform.

⸻

54. Architectural Non-Goals

This integration layer is not intended to:

* turn Raspberry Pi devices into Zorro boards;
* route native interrupts through Paula;
* make Rigel aware of VC4 internals;
* make Rigel aware of Emu68 JIT internals;
* reproduce chipset timing in Bellatrix;
* expose Bellatrix internals to Rigel;
* require classic Amiga hardware for Bellatrix boot;
* make the compatibility layer responsible for native platform discovery.

⸻

55. Integration Invariants

The following rules are normative.

Dependency direction

Bellatrix MAY depend on the public Rigel API.

Rigel MUST NOT depend on Bellatrix internals.

MMIO ownership

Bellatrix MAY route classic MMIO transactions.

Bellatrix MUST NOT implement the semantics of Rigel-owned registers.

Timing ownership

Bellatrix MUST provide execution progress.

Rigel MUST interpret that progress as classic chipset time.

Wall clock

Rigel chipset correctness MUST NOT depend on host wall-clock time.

Interrupt ownership

Rigel MUST own classic INTENA/INTREQ state.

Bellatrix MUST own native interrupt state.

IPL arbitration

Bellatrix MUST arbitrate CPU-visible IPL without merging the underlying interrupt domains.

DMA ownership

Rigel MUST own chipset DMA semantics.

Bellatrix MUST provide access to guest memory without reproducing chipset address-generation behavior.

Host independence

Rigel MUST NOT depend on Raspberry Pi-specific services.

Harness equivalence

The standalone harness MUST exercise the same production librigel interface used by Bellatrix.

Optionality

Disabling Rigel MUST leave a functional Bellatrix Core platform.

⸻

56. Minimal Initial Integration

The first implementation does not need to implement every possible optimization.

The minimum viable boundary is:

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
    └── next event
Interrupt
    │
    └── current IPL
Memory
    │
    ├── DMA read
    └── DMA write

Conceptually:

struct rigel *rigel_create(...);
void rigel_reset(...);
void rigel_destroy(...);
uint16_t rigel_read16(...);
void rigel_write16(...);
void rigel_advance(...);
rigel_time_t rigel_next_event(...);
unsigned rigel_get_ipl(...);

plus the minimum host memory callbacks required for DMA.

Video, audio, diagnostics, and optimized memory mappings may be layered onto this boundary without changing its architectural direction.

⸻

57. Recommended Implementation Order

The initial implementation should proceed in dependency order:

1. Extract/build librigel independently
          │
2. Define opaque Rigel instance
          │
3. Define host memory callbacks
          │
4. Implement standalone harness
          │
5. Define MMIO API
          │
6. Attach Bellatrix MMIO adapter
          │
7. Define execution-progress unit
          │
8. Implement advance/next-event contract
          │
9. Expose Rigel IPL
          │
10. Implement Bellatrix IPL arbitration
          │
11. Validate DMA coherency
          │
12. Attach video/audio presentation

The standalone harness should appear early in this sequence rather than after Bellatrix integration.

That makes librigel independence a property demonstrated by the implementation rather than merely stated by the architecture.

⸻

58. Validation Criteria

The integration should not be considered complete merely because AROS boots with Rigel enabled.

At minimum, validation should demonstrate:

Core independence

CONFIG_RIGEL=n

boots and operates normally.

Rigel independence

The standalone harness builds and operates without Bellatrix.

MMIO isolation

Classic chipset register behavior exists only inside Rigel.

Interrupt isolation

Native interrupts operate without INTENA/INTREQ.

Rigel interrupts operate without entering the BCM interrupt controller path.

Timing ownership

Changing Bellatrix host pacing does not alter deterministic Rigel chipset behavior for an identical execution-progress sequence.

DMA correctness

Rigel DMA and M68K CPU accesses observe a coherent guest memory model.

Reset correctness

Rigel can be reset without reconstructing unrelated native platform state.

Build isolation

librigel can be built without Bellatrix or Raspberry Pi-specific headers.

⸻

59. Review Criteria

Future patches affecting Bellatrix/Rigel integration should be reviewed against the following questions:

1. Does this code belong to the native platform or to classic hardware compatibility?
2. Is Bellatrix learning internal Rigel semantics?
3. Is Rigel acquiring a dependency on Bellatrix?
4. Is native hardware being represented unnecessarily through Amiga hardware mechanisms?
5. Is chipset timing being computed outside Rigel?
6. Is native interrupt state entering INTENA/INTREQ?
7. Is Rigel interrupt state entering the BCM interrupt domain?
8. Is chipset DMA behavior being duplicated in the host?
9. Does the change preserve standalone Rigel testing?
10. Does CONFIG_RIGEL=n remain a first-class supported configuration?

If a patch cannot answer these questions cleanly, the integration boundary should be reconsidered before the patch is accepted.

⸻

60. Final Integration Model

The complete relationship is:

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
             │                              Host Memory API
             │                                     │
             └──────────────────── Guest Memory ───┘

CPU interrupt delivery remains:

Native hardware
      │
      ▼
native_ipl ─────────────┐
                        │
                        ▼
                   IPL arbitration
                        │
                        ▼
                      M68K
                        ▲
                        │
rigel_ipl ──────────────┘
      ▲
      │
   librigel

Neither interrupt domain owns the other.

⸻

61. Integration Definition

The Bellatrix/Rigel integration can be summarized as follows:

Bellatrix hosts Rigel; it does not implement Rigel.

Rigel models classic Amiga hardware; it does not implement the Bellatrix platform.

Bellatrix provides:

* execution;
* address dispatch;
* guest memory;
* native hardware;
* native interrupts;
* presentation services.

Rigel provides:

* classic chipset semantics;
* classic MMIO;
* chipset timing;
* chipset DMA;
* classic interrupt state;
* compatibility hardware output.

The adapter connects these domains without merging them.

This boundary is the implementation contract that preserves the architecture defined by Bellatrix.md.
