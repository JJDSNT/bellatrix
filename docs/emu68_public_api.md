# Emu68 Public Machine Integration API

This document defines the public contract through which a machine host uses
Emu68 as a 68k CPU engine. Bellatrix is the immediate consumer, but no public
type or operation is Bellatrix-, Rigel-, Amiga-, PiStorm-, or ARM-platform-
specific.

The API defined here is not a wrapper around Emu68's page-fault handlers. The
JIT must decide whether an access is direct, external, or unmapped before it
emits an operation that can touch host memory. A normal external access reaches
the host explicitly and never passes through Data Abort.

## 1. Motivation

### Host and CPU responsibilities

Emu68 is a high-performance Motorola 680x0-to-AArch64 dynamic binary translator.
Its historical PiStorm environment tightly integrates the translated CPU with a
physical Amiga bus, Raspberry Pi GPIO, an FPGA protocol, ARM exception handling,
and platform interrupt state. That is a valid accelerator architecture, but it
is not a generic machine-emulator API.

A machine host has separate owners for CPU execution, memory topology, chipset
state, devices, scheduling, and physical host I/O. For Bellatrix the ownership
boundary is:

```text
Bellatrix host
    ARM vector table and physical IRQ/FIQ
    timers, USB, Bluetooth, UART and audio
    inter-core communication and machine scheduling
    guest memory topology

Emu68
    68k instruction execution
    architectural CPU state
    68k exceptions, interrupt-mask semantics and STOP

Rigel
    Amiga chipset state
    generation of the guest 68k interrupt level
```

Emu68 must be usable as the CPU component without taking ownership of
`VBAR_EL1`, physical ARM IRQ/FIQ routing, the interrupt controller, timers, or
host devices.

This ownership boundary does not make migration of the existing physical
exception and IRQ infrastructure part of the API implementation. That migration
is a separate host-integration change and may happen after the API is complete.
During the transition the legacy infrastructure may still be present, but no
operation in this API may depend on it for normal CPU execution, external
access, bounded return, STOP, or guest IPL delivery.

ARM interrupts and 68k interrupts are different contracts. A physical IRQ is a
host-platform event. IPL is a persistent architectural input to the emulated
68k CPU. For example:

```text
USB IRQ
    -> Bellatrix services the physical device
    -> emulated machine state changes
    -> Rigel may publish a new guest interrupt level
    -> Bellatrix calls emu68_machine_set_ipl()
    -> Emu68 applies 68k interrupt semantics at a valid CPU boundary
```

The public API expresses the last two steps without exposing `M68KState.INT` and
without asserting a physical ARM interrupt.

### Guest MMIO is not a host fault

The current Bellatrix bridge is entered after translated native loads or stores
touch an unmapped or protected address. The resulting synchronous Data Abort is
decoded by Emu68 and converted into a machine access. This preserves a cheap RAM
path, but makes normal guest device activity depend on the ARM exception path.

That dependency conflicts with a host which must own the exception
infrastructure for real faults and physical services. The required rule is:

```text
normal guest MMIO access != ARM Data Abort
```

In the machine-integration profile, the JIT recognizes external regions before
emitting the memory operation and generates an explicit host bridge or an
explicit cooperative exit. Data Abort remains a real host fault and diagnostic
mechanism. It is not a bus-dispatch technique.

### The MMU remains part of Emu68

Removing fault-based bus dispatch does not mean removing the AArch64 MMU. Emu68
uses the MMU to map direct guest RAM and ROM, JIT memory and aliases; apply
protection and cacheability attributes; reserve address space; and support
virtual-to-physical translation and translation-cache management.

The separation is exact:

```text
MMU
    direct RAM/ROM mappings
    JIT mappings and protection
    aliases and cache attributes

JIT machine integration
    direct/external/unmapped classification
    explicit external access
    guest bus-error semantics
```

`mmu_map()`, the TTBR tables, `put_4k_page()`, `put_2m_page()`,
`mirror_page()`, and the direct RAM/ROM mappings remain. Only the use of an
absent or protected page as the normal external-bus selector is removed from
the machine profile.

### Direct memory must stay fast

Sending every guest memory access through a C callback would discard a central
benefit of Emu68. The API therefore makes the memory topology explicit and the
JIT provides two deliberate paths:

```text
DIRECT region
    native AArch64 load/store using the mapped fast path

EXTERNAL region
    explicit synchronous callback or cooperative execution exit

UNMAPPED address
    defined 68k bus-error/unmapped behavior
```

There is no generic callback, queue, exception, or address lookup on a
translation-specialized direct access. A dynamic address may require a generated
region check, but a direct result still continues to the native load/store.

### Synchronous and cooperative hosts

A single-core machine may complete an external read or write in a callback and
immediately resume translated execution. A multicore machine may instead run
Emu68 for a bounded interval, receive an external-access stop, service the
access in the component that owns the device, complete it, and resume the CPU.

Both execution modes use the same memory-region and access descriptors. The API
does not expose the host's queues, locks, atomics, IPIs, `wfe`, `sev`, or core
numbers.

PiStorm32's two transaction slots demonstrate transport-level pipelining: reads
wait for data, while some writes remain pending until ordering requires
completion. That optimization remains inside the PiStorm adapter. The public
machine contract defines deterministic ordering and does not expose FPGA slots
or GPIO protocol state.

### Stable public boundary

The host must not depend directly on:

- `struct M68KState` or `__m68k_state`;
- pinned JIT registers or translation-unit layout;
- `SYSPageFaultReadHandler()` or `SYSPageFaultWriteHandler()`;
- `SYSReadValFromAddr()` or `SYSWriteValToAddr()`;
- PiStorm `ps_read_*`, `ps_write_*`, registers, or transaction slots;
- `ESR_EL1`, `FAR_EL1`, ARM vector layout, or wakeup instructions.

An opaque, versioned API lets Emu68 change those internals without changing the
machine host's ABI.

## 2. How Emu68 Works

### Translation and native execution

`emu68/src/ExecutionLoop.c:MainLoop()` loads the global `M68KState`, keeps
important 68k values in pinned AArch64 registers, locates or creates translation
units, and executes native code from the JIT cache. The upstream runtime is a
singleton, historically non-returning execution loop rather than an interpreter
which naturally returns after every instruction.

Guest memory operands converge on
`emu68/src/M68k_EA.c:EMIT_LoadFromEffectiveAddress()` and
`EMIT_StoreToEffectiveAddress()`. These functions calculate 68k effective
addresses and emit native AArch64 `ldr*` and `str*` forms for the different
addressing modes. Consequently, mapped guest memory is accessed without a C
callback.

Bellatrix currently patches `MainLoop()` with safe return points and wraps it in
`src/cpu/emu68/mainloop_window.S:MainLoopWindow()`. The adapter reports the
retired instruction counter held by the JIT and can return on a host stop,
guest STOP, a synchronization marker, or an estimated budget boundary. This
proves that bounded execution is possible, although the public API must make the
run boundary a native Emu68 facility instead of a Bellatrix wrapper.

### MMU layout and guest-address aliases

`emu68/src/aarch64/mmu.c` manages TTBR0 and TTBR1 tables. Its relevant entry
points include `mmu_map()`, `mmu_virt2phys()`, `put_2m_page()`, and
`put_4k_page()`. `mirror_page()` mirrors a low 0..4 GiB mapping into the 4..8
GiB and topmost -4..0 GiB virtual ranges:

```c
/* For 0..4GB create a shadow in the 4..8GB and -4..0GB areas */
```

The current fault handlers normalize these aliases before bus dispatch. The
explicit classifier must do the same before looking up a guest region:

```c
uint32_t guest_address = (uint32_t)computed_address;
```

Thus `0x00000000xxxxxxxx`, `0x00000001xxxxxxxx`, and
`0xffffffffxxxxxxxx` aliases name the same 32-bit guest address when their low
bits match.

In the current Bellatrix path, the MMU has two roles:

1. map directly accessible guest memory;
2. detect external regions by leaving their pages absent or protected.

The public machine profile retains the first role. Region metadata and JIT
code generation replace the second.

### Current fault-based external access

The present Bellatrix flow is:

```text
EMIT_LoadFromEffectiveAddress()/EMIT_StoreToEffectiveAddress()
    -> emit a native LDR/STR
    -> absent/protected mapping causes synchronous Data Abort
    -> vectors.c decodes the faulting AArch64 instruction
    -> SYSPageFaultReadHandler()/SYSPageFaultWriteHandler()
    -> SYSReadValFromAddr()/SYSWriteValToAddr()
    -> emu68_api_dispatch_bus_access()
    -> registered Bellatrix callback
    -> bellatrix_bus_access() fallback when the wrapper does not handle it
    -> handler advances ELR and translated execution resumes
```

`patches/0002-add-bellatrix-bus-hook.patch` installs the Bellatrix page-fault
bridge. `patches/0021-emu68-public-bus-dispatch.patch` adds the current API
dispatcher inside that bridge.

That dispatcher is the architectural problem, not the solution: it wraps the
path from which the machine API must become independent. The new public API does
not call it, does not fall back to it, and is not implemented in `vectors.c`.
Its integration point is the JIT memory emitter before any potentially faulting
host access is emitted.

### PiStorm physical-bus backend

For PiStorm, `emu68/src/aarch64/vectors.c` dispatches bus faults to
`ps_read_8/16/32/64/128()` and `ps_write_8/16/32/64/128()` from
`emu68/src/pistorm/ps_protocol.*`. These functions encode FPGA/GPIO
transactions, access size, function-code state, waits, and device-specific
ordering.

`emu68/src/pistorm/ps32_protocol.c` maintains `next_slot` and
`slot_active[2]`. Reads wait for a returned value. Some writes may stay active
until a slot is reused, while chipset address ranges force completion. This is
synchronous CPU integration with transport-level pipelining, not a generic
asynchronous API.

The PiStorm backend remains a supported Emu68 platform backend. It can retain
its current fault/physical-bus implementation or use an internal adapter. Its
GPIO registers, slots, blitter waits, and address-specific policies never enter
the public machine API.

### Interrupt delivery and STOP

`emu68/src/ExecutionLoop.c:MainLoop()` consumes the internal interrupt fields.
The Bellatrix path publishes the guest level through `M68KState.INT.IPL`,
compares it with the SR interrupt mask, and creates the 68k interrupt exception
frame when eligible. `INT.ARM` is a separate PiStorm physical-line concept and
is deliberately not used as the Bellatrix guest level.

The current internal behavior demonstrates the required semantics: validate a
level in 0..7, update the persistent guest IPL, perform memory ordering, and
wake execution if necessary without presenting an ARM IRQ to the JIT. The
public operation exposes only those 68k semantics; the internal field writes and
wakeup instruction are implementation details.

The Bellatrix patch to `emu68/src/M68k_LINE4.c` records an architectural STOP
state rather than making guest STOP depend on a physical interrupt. At a run
boundary, Emu68 remains stopped while the published IPL is masked, and resumes
68k interrupt delivery when an eligible level exists. PiStorm's existing
`wfe`/physical-interrupt behavior remains confined to its platform path.

### Existing public-looking wrapper

`src/cpu/emu68/emu68_api.h` and
`src/cpu/emu68/emu68_api_adapter.c` currently provide an opaque singleton
handle, lifecycle functions, bounded-run wrappers, bus callbacks, guest IPL,
partial state access, invalidation, events, and statistics.

This interface is not the API defined by this document. In particular:

- its bus dispatcher is called from the page-fault path;
- it has no direct/external/unmapped region registry;
- it exposes a Bellatrix-owned singleton wrapper over global Emu68 state;
- its synchronization result is a delayed run marker, not a pending-access
  protocol;
- it labels the live fault traffic as data space even when the underlying 68k
  function code is not represented.

The implementation uses the public contract below directly. It does not extend
or layer another wrapper over the current fault dispatcher.

## 3. How the Public API Works

### Public header and ABI

The public header is `src/cpu/emu68/emu68_machine.h` in the embedding
repository. It contains no Bellatrix headers and is usable by any machine host.
The runtime lives beside it; only the minimal JIT integration hooks are carried
as Emu68 patches. All CPU internals remain behind an opaque handle:

```c
#define EMU68_MACHINE_ABI_VERSION 1u

typedef struct emu68_cpu emu68_cpu_t;
```

Every extensible public structure begins with `abi_version` and `struct_size`.
The caller zero-initializes the whole structure, sets both fields, and fills the
members it uses. Emu68 rejects an unsupported ABI version or a structure smaller
than the version's required prefix. It ignores a larger trailing area so the ABI
can remain binary-compatible.

ABI version 1 provides the complete machine-integration contract described in
this section: lifecycle, memory topology, synchronous and cooperative external
access, bounded execution, reset and stop, guest IPL, and code invalidation.

### Types and configuration

```c
typedef enum emu68_execution_mode {
    EMU68_EXEC_SYNCHRONOUS = 0,
    EMU68_EXEC_COOPERATIVE = 1
} emu68_execution_mode_t;

typedef enum emu68_status {
    EMU68_OK = 0,
    EMU68_ERR_INVALID_ARGUMENT,
    EMU68_ERR_ABI_MISMATCH,
    EMU68_ERR_BUSY,
    EMU68_ERR_OVERLAP,
    EMU68_ERR_NOT_FOUND,
    EMU68_ERR_ACCESS,
    EMU68_ERR_INTERNAL
} emu68_status_t;

typedef struct emu68_machine_ops emu68_machine_ops_t;

typedef struct emu68_machine_config {
    uint32_t abi_version;
    size_t struct_size;
    emu68_execution_mode_t execution_mode;
    const emu68_machine_ops_t *ops;
    void *opaque;
} emu68_machine_config_t;

emu68_status_t emu68_machine_create(
    const emu68_machine_config_t *config,
    emu68_cpu_t **out_cpu);

void emu68_machine_destroy(emu68_cpu_t *cpu);
```

The handle owns all CPU-visible state, region metadata, pending-access state,
and JIT cache state associated with the CPU. The Emu68 runtime supports one live
handle, so a second create returns `EMU68_ERR_BUSY`. The singleton is never
visible in the ABI.

The `opaque` pointer belongs to the host and is passed unchanged to callbacks.

### Memory topology

The host completely describes the 32-bit guest address space with non-overlapping
regions while the CPU is not running:

```c
typedef enum emu68_region_kind {
    EMU68_REGION_DIRECT = 0,
    EMU68_REGION_EXTERNAL = 1,
    EMU68_REGION_UNMAPPED = 2
} emu68_region_kind_t;

enum {
    EMU68_REGION_READ      = 1u << 0,
    EMU68_REGION_WRITE     = 1u << 1,
    EMU68_REGION_EXECUTE   = 1u << 2,
    EMU68_REGION_CACHEABLE = 1u << 3
};

typedef struct emu68_direct_region {
    uint32_t abi_version;
    size_t struct_size;
    uint32_t guest_base;
    uint64_t size;
    void *host_base;
    uint32_t flags;
} emu68_direct_region_t;

typedef struct emu68_external_region {
    uint32_t abi_version;
    size_t struct_size;
    uint32_t guest_base;
    uint64_t size;
    uint32_t region_id;
    uint32_t flags;
} emu68_external_region_t;

emu68_status_t emu68_machine_map_direct(
    emu68_cpu_t *cpu, const emu68_direct_region_t *region);

emu68_status_t emu68_machine_map_external(
    emu68_cpu_t *cpu, const emu68_external_region_t *region);

emu68_status_t emu68_machine_map_unmapped(
    emu68_cpu_t *cpu, uint32_t guest_base, uint64_t size);

emu68_status_t emu68_machine_unmap(
    emu68_cpu_t *cpu, uint32_t guest_base, uint64_t size);
```

A region may not wrap past `0xffffffff`, have zero size, or overlap another
region. An uncovered address is UNMAPPED. `region_id` is an opaque value returned
to the host with external accesses; Emu68 does not interpret it.

DIRECT regions require page-aligned guest and host bases and a page-multiple
size. `EMU68_REGION_EXECUTE` is valid only for DIRECT regions because Emu68's
translator consumes instruction bytes from directly mapped memory. An opcode
fetch from any address which is not both DIRECT and executable takes the
defined 68k access-fault path; it never invokes the translator through a host
pointer or an ARM fault.

DIRECT registration uses Emu68's MMU to establish the native mapping with the
requested permissions and attributes. EXTERNAL registration creates JIT-visible
classification metadata but no host mapping. UNMAPPED registration explicitly
selects 68k bus-error semantics. Reclassification first unmaps the old range and
then registers the replacement.

Every topology change invalidates affected translation units and performs the
required TLB/cache maintenance before returning. No translated unit may retain
an access strategy inconsistent with the current region table.

The JIT normalizes every computed address to 32 bits before classification. For
a statically known direct range it emits the existing native access. For a
statically known external range it emits the external helper. For a dynamic
address it emits a fast region test and branches to the selected direct,
external, or unmapped path. The external and unmapped branches execute before
any native access to the guest address.

Classification covers the complete access width, not only its first byte. An
access whose bytes do not all belong to one region with the required permission
takes the defined 68k access-fault path. This also prevents a native DIRECT
operation from crossing into an EXTERNAL or UNMAPPED region.

### External-access descriptor

```c
typedef enum emu68_access_kind {
    EMU68_ACCESS_READ = 0,
    EMU68_ACCESS_WRITE = 1
} emu68_access_kind_t;

typedef enum emu68_address_space {
    EMU68_SPACE_DATA = 0,
    EMU68_SPACE_PROGRAM = 1,
    EMU68_SPACE_CPU = 2
} emu68_address_space_t;

typedef enum emu68_bus_result {
    EMU68_BUS_COMPLETE = 0,
    EMU68_BUS_ERROR = 1
} emu68_bus_result_t;

typedef struct emu68_bus_access {
    uint32_t abi_version;
    size_t struct_size;
    uint64_t sequence;
    uint32_t address;
    uint32_t region_id;
    emu68_access_kind_t kind;
    emu68_address_space_t space;
    uint8_t function_code;
    uint8_t width;          /* 1, 2, 4, 8 or 16 bytes */
    emu68_bus_result_t result;
    uint8_t reserved[2];
    uint64_t value_lo;
    uint64_t value_hi;
} emu68_bus_access_t;
```

`address` is the normalized 32-bit guest address. `sequence` increases for every
external access made by that CPU and identifies a cooperative completion.
`function_code` is the 68k function code used by the access; `space` gives the
corresponding data, program, or CPU-space classification. The translator must
propagate this information rather than label every access as data.

Widths are in bytes. Values use 68k logical byte order: an 8/16/32/64-bit value
is held in the low bits of `value_lo`; a 128-bit value uses `value_lo` for the
low 64 bits and `value_hi` for the high 64 bits. For a write, Emu68 fills the
value before handing the descriptor to the host. For a read, the host fills it
before successful completion.

### Synchronous mode

```c
typedef emu68_bus_result_t (*emu68_bus_access_fn)(
    void *opaque, emu68_bus_access_t *access);

struct emu68_machine_ops {
    uint32_t abi_version;
    size_t struct_size;
    emu68_bus_access_fn bus_access;
};
```

In `EMU68_EXEC_SYNCHRONOUS`, encountering an EXTERNAL region calls
`bus_access()` directly. A successful write completes when the callback returns
`EMU68_BUS_COMPLETE`. A successful read also requires the callback to place the
value in the descriptor. `EMU68_BUS_ERROR` causes the defined 68k bus-error
path; it does not fall back to an ARM fault handler.

The callback runs on the CPU owner's thread/core. It may synchronize with other
host components internally, but translated execution does not continue until it
returns. External accesses are observed in 68k program order. The API performs
no posted writes and no external-access reordering.

The callback must not recursively run or destroy the same CPU, change its region
topology, reset it, mutate its register state, or invalidate code. It may call
the cross-thread-safe IPL and stop-request operations.

### Cooperative mode

```c
typedef enum emu68_stop_reason {
    EMU68_STOP_BUDGET = 0,
    EMU68_STOP_EXTERNAL_ACCESS,
    EMU68_STOP_STOPPED,
    EMU68_STOP_REQUESTED,
    EMU68_STOP_BUS_ERROR,
    EMU68_STOP_FATAL
} emu68_stop_reason_t;

typedef struct emu68_run_result {
    uint32_t abi_version;
    size_t struct_size;
    emu68_stop_reason_t reason;
    uint64_t cycles_executed;
    uint64_t instructions_executed;
    uint32_t pc;
    uint32_t detail;
} emu68_run_result_t;

emu68_status_t emu68_machine_run(
    emu68_cpu_t *cpu,
    uint64_t cycle_budget,
    emu68_run_result_t *result);

emu68_status_t emu68_machine_get_pending_access(
    emu68_cpu_t *cpu,
    emu68_bus_access_t *out_access);

emu68_status_t emu68_machine_complete_access(
    emu68_cpu_t *cpu,
    const emu68_bus_access_t *completion);
```

In `EMU68_EXEC_COOPERATIVE`, an EXTERNAL access suspends translated execution
before the access is performed and returns `EMU68_STOP_EXTERNAL_ACCESS`. The host
gets the descriptor, services it, completes the matching sequence, and calls
`emu68_machine_run()` again.

Exactly one access may be pending per CPU. Both reads and writes suspend. The
descriptor remains stable until completion, reset, or destruction. Completion
must carry the same sequence, address, kind, width, space, function code, and
region ID. For a read it supplies the value. A completion marked as a bus error
enters the 68k bus-error path when execution resumes.

No access is posted or reordered. An interrupt may be published while an access
is pending, but Emu68 completes or faults that access before observing the
interrupt at the next valid 68k boundary. A stop request returns control without
inventing an access result. Reset cancels the pending access.

`cycle_budget` is measured in modeled 68k cycles, not ARM cycles or elapsed wall
time. Emu68 accounts each retired instruction and exception using its 68k cost
model. `cycles_executed` may exceed the requested budget only by the cost of the
instruction that reaches the next safe return boundary. The result also reports
the exact number of retired 68k instructions.

Synchronous mode uses the same run operation; its external accesses complete
inside callbacks and therefore do not return `EMU68_STOP_EXTERNAL_ACCESS`.

### Reset, stop, IPL, and invalidation

```c
typedef struct emu68_reset_state {
    uint32_t abi_version;
    size_t struct_size;
    uint32_t initial_ssp;
    uint32_t initial_pc;
} emu68_reset_state_t;

emu68_status_t emu68_machine_reset(
    emu68_cpu_t *cpu, const emu68_reset_state_t *state);
void emu68_machine_request_stop(emu68_cpu_t *cpu);
emu68_status_t emu68_machine_set_ipl(emu68_cpu_t *cpu, unsigned level);

emu68_status_t emu68_machine_invalidate_code(
    emu68_cpu_t *cpu, uint32_t guest_base, uint64_t size);

emu68_status_t emu68_machine_invalidate_all_code(emu68_cpu_t *cpu);
```

The host owns the machine reset sequence, including reading reset vectors 0 and
4 through its memory system, and supplies their resulting SSP and PC values.
Reset installs those values, restores the remaining architectural reset state,
cancels any pending access, clears STOP and stop-request state, and invalidates
translations as required. It does not reset host devices and cannot itself
create a cooperative external access.

`emu68_machine_request_stop()` asks the running owner to return
`EMU68_STOP_REQUESTED` at the next safe boundary. It does not modify 68k state.

`emu68_machine_set_ipl()` accepts levels 0..7 and rejects other values. The
level is persistent until replaced. Emu68 performs the necessary memory ordering
and wakeup, compares it with the 68k SR mask, and delivers an eligible interrupt
only at an architectural boundary. The operation never asserts an ARM IRQ/FIQ.

Code invalidation is also valid only while the CPU is stopped. Range invalidation
uses normalized guest addresses, covers every translation unit intersecting the
range, and completes instruction-cache maintenance before returning. A topology
change performs this invalidation automatically.

### Thread and ordering contract

One host thread/core owns `emu68_machine_run()`, reset, topology
changes, pending-access completion, invalidation, and destruction for a CPU.
Concurrent run calls return `EMU68_ERR_BUSY`.

Only `emu68_machine_set_ipl()` and `emu68_machine_request_stop()` are callable
from another thread/core while the CPU runs. They use release publication and
the run loop uses acquire observation at safe boundaries. Any wakeup mechanism
needed to make that observation prompt is private to Emu68.

For external accesses, the contract is sequential and conservative:

- accesses are issued in 68k program order;
- reads complete before their value is consumed;
- writes complete before the next external access is issued;
- neither execution mode posts or reorders writes;
- reset cancels a cooperative pending access;
- an interrupt is not delivered in the middle of an incomplete access;
- region changes and code invalidation are complete before the next run.

### Required Emu68 integration

The API is implemented inside Emu68, not around the Bellatrix fault bridge. The
effective-address emitters and their shared load/store helpers must use the
registered region metadata and generate the direct/external/unmapped split.
All load and store forms, including indexed, predecrement, postincrement,
floating-point, and wide operations, must pass through the same classification
rule.

The machine profile is correct when:

- direct RAM/ROM still use native JIT loads and stores;
- external regions reach the public callback or cooperative exit explicitly;
- no normal machine access calls a page-fault or `SYS*ValFromAddr()` handler;
- unmapped access follows defined 68k bus-error semantics;
- aliases are normalized consistently to a 32-bit guest address;
- region changes invalidate every affected translation;
- guest IPL and STOP work without physical ARM interrupts;
- the API neither acquires nor depends on ARM vector, IRQ/FIQ, timer, or device
  ownership; transferring any legacy physical infrastructure to the host is a
  separate implementation;
- PiStorm remains isolated behind its existing platform backend;
- no public type exposes Bellatrix, Rigel, GPIO, exception-frame, MMU-table, or
  JIT-register details.

There is no compatibility fallback from this API to the current Bellatrix
fault dispatcher. Once the machine profile selects this API, Data Abort means a
real host fault or diagnostic condition, never ordinary guest bus traffic.
