# Bellatrix Host Abstraction Architecture

## Platform Independence, Host Services, and Target-Specific Integration

**Status:** Proposed Architectural Baseline  
**Target:** Bellatrix Host Interface Version 1  
**Related documents:**

* `Bellatrix.md`
* `Rigel_integration.md`
* `Rigel_API_Convergence.md`
* Emu68 Host Bus / IPL Patch Plan

---

# 1. Purpose

This document defines the host abstraction boundary required to keep Bellatrix independent from any specific physical platform.

The initial Bellatrix target is the Raspberry Pi 3B running Emu68 bare-metal.

However:

> The Raspberry Pi 3B is a Bellatrix host target. It is not the architectural definition of Bellatrix.

Bellatrix Core must therefore not depend directly on:

* BCM2837-specific hardware;
* Raspberry Pi peripheral addresses;
* Raspberry Pi interrupt controllers;
* Raspberry Pi timers;
* VC4;
* Raspberry Pi firmware interfaces;
* ARM core numbers;
* board-specific cache or MMU implementation details.

These mechanisms belong below an explicit Bellatrix Host Abstraction Layer.

The intended architecture is:

~~~text
                     Bellatrix
                         │
                  Machine / Core
                         │
          ┌──────────────┼──────────────┐
          │              │              │
        Bus             IRQ           Memory
          │              │              │
          └──────────────┼──────────────┘
                         │
                  Host Abstraction
                         │
          ┌──────────────┼──────────────┐
          │              │              │
       RPi3B           Linux          Future
       Target          Target         Target
~~~

The purpose of the host layer is not to hide every possible device behind one universal interface.

Its purpose is to isolate the platform mechanisms required by Bellatrix itself.

---

# 2. Architectural Principle

The fundamental rule is:

> Bellatrix defines machine policy. The host implements platform mechanism.

For example:

~~~text
Bellatrix machine policy

"this guest physical region must be protected"
              │
              ▼
         Host abstraction
              │
              ▼
platform-specific mapping implementation
~~~

Bellatrix may decide:

~~~text
Chip RAM belongs here

MMIO belongs here

this region must fault

this interrupt domain must reach the CPU

translated code covering this memory must be invalidated
~~~

Bellatrix should not need to know:

~~~text
which ARM page-table descriptor is required

which BCM interrupt register must be written

which physical peripheral base belongs to the board

which ARM core performs the operation

which Raspberry Pi firmware call provides a resource
~~~

Those are host implementation details.

---

# 3. Architectural Position

The Host Abstraction Layer exists below Bellatrix machine semantics.

Conceptually:

~~~text
              Bellatrix machine semantics
                         │
                         ▼
                 Host Abstraction
                         │
                         ▼
               Platform implementation
~~~

This boundary complements the Rigel boundary.

The complete architecture becomes:

~~~text
                  Classic Amiga semantics
                           │
                           ▼
                         Rigel
                           │
                           ▼
                       Bellatrix
                           │
                           ▼
                    Host Abstraction
                           │
                           ▼
                    Native Platform
~~~

Bellatrix therefore sits between two intentionally independent domains:

~~~text
              compatibility domain
                      ▲
                      │
                    Rigel
                      ▲
                      │
                  Bellatrix
                      │
                      ▼
                  Host / PAL
                      │
                      ▼
                platform domain
~~~

Rigel must not know how Bellatrix implements the host.

The host must not know how Rigel implements classic Amiga hardware.

Bellatrix coordinates the two.

---

# 4. Bellatrix Core

Bellatrix Core owns machine-level policy.

This includes concepts such as:

* guest physical address-space organization;
* memory-region policy;
* provider registration;
* MMIO provider selection;
* native versus compatibility interrupt arbitration;
* execution coordination;
* optional Rigel integration;
* machine lifecycle;
* guest-visible platform topology.

Bellatrix Core should remain independent from the concrete implementation of the physical host.

Conceptually:

~~~text
Bellatrix Core
│
├── machine
├── address-space policy
├── bus/provider dispatch
├── interrupt arbitration
├── execution coordination
├── guest-memory topology
└── optional compatibility integration
~~~

It should not become a Raspberry Pi hardware support package.

---

# 5. Host Abstraction Layer

The host abstraction provides mechanisms Bellatrix requires from its execution environment.

Conceptually:

~~~text
Bellatrix
    │
    ▼
Host API
    │
    ├── memory mapping
    ├── memory protection
    ├── execution/fault integration
    ├── native interrupt plumbing
    ├── timing primitives
    ├── cache / execution coherency
    ├── host execution primitives
    ├── platform discovery
    └── diagnostics
~~~

The host interface should expose capabilities rather than board-specific implementation details.

Bellatrix should request:

~~~text
map this region

protect this region

register this bus/fault path

invalidate translated execution for this range

obtain native interrupt state
~~~

rather than:

~~~text
modify this BCM register

install this ARM vector

use this Raspberry Pi peripheral base

send SEV to ARM core 2
~~~

---

# 6. Host Abstraction Is Not a Device-Driver Dumping Ground

The host abstraction must remain deliberately small.

It should not become a generic collection of every platform-specific service.

The distinction should be:

~~~text
Host / PAL
    │
    ├── execution environment
    ├── memory mechanisms
    ├── MMU / mapping mechanisms
    ├── interrupt plumbing
    ├── timing primitives
    ├── cache / coherency primitives
    └── platform discovery

Device backends
    │
    ├── storage
    ├── video
    ├── audio
    ├── USB
    ├── network
    └── other devices
~~~

A storage controller does not become part of the Host API merely because the first implementation happens to use Raspberry Pi hardware.

Likewise, VC4 presentation should not become a fundamental Bellatrix host primitive unless Bellatrix Core genuinely requires that abstraction.

---

# 7. Proposed Source Organization

A possible organization is:

~~~text
src/
│
├── machine/
│   ├── machine.c
│   ├── machine.h
│   ├── memory.c
│   ├── memory.h
│   ├── bus.c
│   └── bus.h
│
├── irq/
│   ├── irq.c
│   └── irq.h
│
├── amiga/
│   ├── rigel_adapter.c
│   ├── rigel_adapter.h
│   ├── bus.c
│   └── irq.c
│
├── host/
│   ├── host.h
│   │
│   ├── rpi3b/
│   │   ├── host.c
│   │   ├── memory.c
│   │   ├── irq.c
│   │   ├── timer.c
│   │   ├── cache.c
│   │   └── platform.c
│   │
│   ├── linux/
│   │   └── ...
│   │
│   └── future/
│       └── ...
│
└── drivers/
    ├── storage/
    ├── video/
    ├── audio/
    ├── usb/
    └── network/
~~~

The exact directory structure is not normative.

The important architectural property is:

> Platform-specific mechanisms must have an explicit ownership boundary.

---

# 8. Machine Policy Versus Host Mechanism

The separation between `machine/` and `host/` should be strict.

For example:

~~~text
machine.c

decides:

0x000000–0xFFFFFF
starts protected
~~~

The host performs:

~~~text
host_memory_protect(...)
~~~

Similarly:

~~~text
machine.c

decides:

Chip RAM should become directly accessible
~~~

The host performs:

~~~text
host_memory_map(...)
~~~

The machine layer therefore expresses policy.

The host layer implements that policy using whatever mechanism exists on the target.

---

# 9. Memory Ownership

Bellatrix owns guest-memory topology.

The host provides the mechanism used to implement that topology.

Conceptually:

~~~text
Bellatrix
    │
    │ guest physical topology
    ▼
Machine memory policy
    │
    ▼
Host memory interface
    │
    ▼
platform MMU / mapping implementation
~~~

Bellatrix may define:

~~~text
Chip RAM

normal RAM

protected low-24 regions

MMIO regions

native platform regions
~~~

The host decides how those policies are represented physically.

---

# 10. Guest Physical Memory Must Remain Platform-Neutral

Bellatrix should reason in terms of guest physical addresses.

For example:

~~~text
guest physical address
        │
        ▼
Bellatrix memory policy
        │
        ▼
Host implementation
        │
        ▼
host physical / virtual representation
~~~

A guest physical address must not implicitly mean:

~~~text
ARM physical address

Raspberry Pi bus address

host pointer

VC4 bus address
~~~

These are separate namespaces.

The host implementation is responsible for translating Bellatrix requirements into the native representation where necessary.

---

# 11. MMU and Mapping

The host abstraction should expose the mapping capabilities required by Bellatrix without exposing target-specific MMU details.

Conceptually:

~~~text
Bellatrix
   │
   ├── map
   ├── unmap
   ├── protect
   └── query
          │
          ▼
       Host API
          │
          ▼
 platform MMU implementation
~~~

For the Raspberry Pi 3B target, this may ultimately manipulate ARM translation tables used by the Emu68 environment.

Another host may implement the same Bellatrix requirement differently.

Bellatrix Core must not depend on the implementation mechanism.

---

# 12. Fault and Bus Integration

Bellatrix requires a mechanism for trapping accesses that cannot be handled as ordinary directly mapped memory.

Conceptually:

~~~text
M68K access
     │
     ▼
execution engine
     │
     ▼
host fault / bus mechanism
     │
     ▼
Bellatrix address dispatcher
     │
     ├── mapped memory
     ├── native provider
     ├── Rigel provider
     └── unmapped
~~~

The host abstraction should expose the mechanism required to connect Bellatrix's address dispatcher to the execution environment.

The host implementation may use:

* Emu68 fault hooks;
* another emulator callback;
* operating-system memory faults;
* explicit software dispatch;
* another mechanism.

Bellatrix should not depend on which one is used.

---

# 13. Relationship with the Emu68 Host Bus Hook

For the initial Raspberry Pi 3B target:

~~~text
Emu68
   │
   ▼
generic host bus hook
   │
   ▼
Bellatrix RPi3B host
   │
   ▼
Bellatrix address dispatcher
~~~

The generic Emu68 host-bus patch therefore belongs conceptually to the host integration boundary.

It should not cause Bellatrix machine semantics to depend directly on Emu68 internals.

Conceptually:

~~~text
Emu68 mechanism
      │
      ▼
RPi3B / Emu68 host implementation
      │
      ▼
generic Bellatrix bus contract
      │
      ▼
Bellatrix machine
~~~

This makes the Emu68 patch an implementation mechanism rather than an architectural dependency throughout Bellatrix.

---

# 14. Provider Selection

Bellatrix remains responsible for machine-level provider selection.

Conceptually:

~~~text
M68K address
     │
     ▼
Bellatrix address dispatcher
     │
     ├── RAM
     ├── native device
     ├── Rigel compatibility region
     └── unmapped
~~~

The host provides the mechanism through which the access reaches Bellatrix.

The host must not interpret Rigel registers.

Rigel must not become the global Bellatrix address dispatcher.

The boundary is:

~~~text
execution environment
        │
        ▼
      Host
        │
        ▼
Bellatrix dispatcher
        │
        ▼
provider
~~~

---

# 15. Native Interrupt Domain

Bellatrix distinguishes native platform interrupts from Rigel compatibility interrupts.

Conceptually:

~~~text
Native platform
      │
      ▼
Host interrupt implementation
      │
      ▼
native_ipl
      │
      ┐
      │
      ├──► Bellatrix IPL arbitration ──► M68K
      │
      ┘
rigel_ipl
      ▲
      │
    Rigel
~~~

The host is responsible for delivering native interrupt information to Bellatrix.

Rigel is responsible for delivering compatibility-domain IPL to Bellatrix.

Bellatrix arbitrates between the domains.

---

# 16. Host IRQ Ownership

Board-specific interrupt-controller knowledge belongs to the host target.

For the Raspberry Pi 3B this may include:

~~~text
BCM283x interrupt controller

pending banks

enable/disable registers

platform IRQ numbering

ARM interrupt routing
~~~

Bellatrix Core should not need to know these details.

Instead:

~~~text
RPi3B interrupt controller
           │
           ▼
      host/rpi3b/irq.c
           │
           ▼
     generic native IRQ
           │
           ▼
        Bellatrix
~~~

Another target may use a completely different interrupt controller.

---

# 17. Relationship with Emu68 INTF.ARM

The existing Emu68 native host interrupt path should remain a host mechanism.

For the initial target:

~~~text
physical host IRQ
      │
      ▼
Emu68 INTF.ARM
      │
      ▼
M68K native interrupt path
~~~

Bellatrix should preserve this native domain.

It must not reinterpret native host interrupts as Rigel interrupts.

The architectural distinction remains:

~~~text
INTF.ARM
    │
    └── native host domain


INTF.IPL
    │
    └── classic compatibility IPL
~~~

The Host API should preserve this distinction rather than hide it behind one generic interrupt source.

---

# 18. Rigel IPL Is Not a Host Interrupt

Rigel owns classic Amiga interrupt semantics.

Conceptually:

~~~text
Rigel
 │
 ├── INTREQ
 ├── INTENA
 ├── classic sources
 └── priority resolution
          │
          ▼
       rigel_ipl
          │
          ▼
      Bellatrix
~~~

The host does not calculate this value.

The host merely provides the platform mechanism through which the final Bellatrix-selected IPL reaches execution where necessary.

This preserves the ownership boundary:

~~~text
Rigel
    owns compatibility interrupt semantics

Bellatrix
    owns cross-domain arbitration

Host
    owns delivery mechanism
~~~

---

# 19. Timing

The host may provide native timing primitives.

These must not become authoritative Rigel time.

Conceptually:

~~~text
Host timer
    │
    └── native platform service


Bellatrix execution accounting
    │
    └── CPU execution coordination


Rigel timeline
    │
    └── authoritative chipset time
~~~

The Host API may therefore provide something conceptually equivalent to:

~~~c
uint64_t
host_counter(...);
~~~

but the existence of such a counter must not imply:

~~~text
Rigel time = host wall clock
~~~

Rigel's deterministic virtual timeline remains independently owned.

---

# 20. Cache and Execution Coherency

One host responsibility deserves an explicit abstraction: executable-memory coherency.

Rigel or another DMA-capable provider may modify guest memory.

Bellatrix may know that this memory contains translated M68K code.

The resulting relationship should be:

~~~text
Rigel DMA
    │
    ▼
guest physical memory modified
    │
    ▼
Bellatrix memory backend
    │
    ▼
host execution coherency operation
    │
    ▼
Emu68 translation state updated
~~~

Rigel must not know:

~~~text
Emu68 JIT internals

translation-cache structures

ARM instruction cache details
~~~

Bellatrix should request the required semantic operation.

The host implements it.

---

# 21. Host Context

The host interface should support an opaque host context.

Conceptually:

~~~c
struct bellatrix_host_ops {
    ...
};

struct bellatrix_host {
    const struct bellatrix_host_ops *ops;
    void *context;
};
~~~

The exact representation should not be frozen prematurely.

The important property is:

> Bellatrix Core does not inspect target-specific host state.

For example, the Raspberry Pi host context might internally contain:

~~~text
MMU state

Emu68 integration state

interrupt-controller state

platform description

timer state

core coordination state
~~~

None of those structures should become Bellatrix Core dependencies.

---

# 22. Multicore Ownership

Core assignment and host execution topology belong to the host.

Bellatrix may express semantic requirements such as:

~~~text
run this work

serialize this operation

wake execution

wait for host work

publish this state safely
~~~

It should not encode:

~~~text
run Rigel on Core 2

send SEV to Core 3

use Core 1 for video

Core 0 owns this queue
~~~

Those are host policy and implementation decisions.

The same principle applies to Rigel.

Rigel may support being executed according to a host-selected threading strategy.

It must not select ARM cores itself.

---

# 23. Rigel and Host Context Awareness

Rigel should remain host-independent while still supporting an opaque host context.

Conceptually:

~~~text
Rigel
  │
  ▼
host operation
  │
  ▼
opaque host context
  │
  ▼
Bellatrix adapter
  │
  ▼
Bellatrix / Host
~~~

This allows the host to associate operations with whatever state it requires without exposing that state to Rigel.

The distinction is:

~~~text
host context awareness
        ≠
host implementation awareness
~~~

Rigel may know that an opaque context exists.

Rigel must not know what that context contains.

---

# 24. Video Ownership

Video requires a particularly clear boundary.

Rigel owns classic video-generation semantics.

Bellatrix owns presentation policy.

The video backend owns native presentation mechanism.

Conceptually:

~~~text
Denise / Agnus
      │
      ▼
    Rigel
      │
      ▼
host-independent video output
      │
      ▼
Bellatrix video adaptation
      │
      ▼
video backend
      │
      ▼
VC4 / framebuffer / other target
~~~

Rigel must not know about:

~~~text
VC4

DispmanX

mailbox interfaces

HDMI

Linux DRM

native framebuffer addresses
~~~

Likewise, Bellatrix machine semantics should not need to know the detailed VC4 programming model.

---

# 25. Video Output Is Not RTG

The Rigel video interface should represent the output of classic chipset video generation.

It should not recreate an RTG device.

Conceptually:

~~~text
classic chipset state
        │
        ▼
raster generation
        │
        ▼
host-independent Rigel video representation
        │
        ▼
Bellatrix presentation
~~~

This distinction is important because native AROS graphics and classic Amiga compatibility video are separate domains.

Bellatrix may have a native graphics path independent from Rigel.

Therefore:

~~~text
Native AROS graphics
        │
        ▼
native Bellatrix video path
        │
        ▼
video backend


Rigel classic graphics
        │
        ▼
Rigel video output
        │
        ▼
Bellatrix compatibility presentation
        │
        ▼
video backend
~~~

Both may eventually reach the same physical display backend without becoming the same graphics architecture.

---

# 26. Host-Independent Rigel Video Representation

The precise Rigel video representation should be selected according to the needs of the chipset model and host integration.

Possible representations include:

~~~text
completed framebuffer

completed frame

scanlines

incremental raster output

raster events

another deterministic host-independent representation
~~~

The critical requirement is:

> The representation must describe Rigel output, not the native GPU used to display it.

For example, an eventual representation might conceptually contain:

~~~text
frame dimensions

pixel format

visible region

scanline information

frame/raster timing information

pixel or surface data
~~~

without containing:

~~~text
VC4 resource handle

ARM framebuffer pointer

Linux DRM object

Raspberry Pi mailbox object
~~~

---

# 27. Video Backend

Native presentation should use a separate backend boundary.

Conceptually:

~~~text
Bellatrix presentation layer
          │
          ▼
      Video Backend API
          │
      ┌───┼───────────────┐
      │   │               │
     VC4 framebuffer    future
~~~

The RPi3B target may provide a VC4-based backend.

Another platform may provide:

~~~text
DRM/KMS

SDL

UEFI GOP

native framebuffer

another GPU API
~~~

without requiring changes to Rigel.

---

# 28. Storage

Storage should not be part of the minimal Host API unless Bellatrix Core itself requires a generic storage primitive.

Instead:

~~~text
Bellatrix
    │
    ▼
storage subsystem
    │
    ▼
storage backend
    │
    ▼
platform implementation
~~~

For the Raspberry Pi:

~~~text
AROS / Bellatrix storage
          │
          ▼
       SD backend
          │
          ▼
      Raspberry Pi SD
~~~

Another host may use:

~~~text
file-backed disk

NVMe

USB storage

virtual block device
~~~

without modifying Bellatrix machine semantics.

---

# 29. Input

Input follows a similar pattern.

Conceptually:

~~~text
native input device
       │
       ▼
input backend
       │
       ▼
Bellatrix input model
       │
       ├── native guest input
       │
       └── compatibility adaptation
                    │
                    ▼
                  Rigel
~~~

Rigel should receive classic hardware-facing input state.

It should not receive native transport details such as:

~~~text
USB HID descriptor

Bluetooth connection object

RPi GPIO implementation state
~~~

---

# 30. Platform Discovery

Platform discovery belongs to the host implementation.

A target may use:

~~~text
FDT

ACPI

UEFI

fixed board description

operating-system APIs

another discovery mechanism
~~~

Bellatrix should consume normalized information where machine policy genuinely requires it.

For the Raspberry Pi target:

~~~text
FDT
 │
 ▼
RPi3B host discovery
 │
 ▼
normalized host/platform information
 │
 ▼
Bellatrix
~~~

Bellatrix Core should not become an FDT parser merely because the first target uses FDT.

---

# 31. FDT Is a Discovery Mechanism, Not the Architecture

The architectural contract must not require FDT.

Instead:

~~~text
RPi3B
  │
  └── may use FDT

another board
  │
  └── may use FDT

UEFI host
  │
  └── may use firmware tables

Linux host
  │
  └── may use OS interfaces
~~~

Bellatrix depends on host capabilities.

It does not depend on the mechanism through which the host discovered those capabilities.

---

# 32. Emu68 Ownership

Emu68 should be treated as part of the execution environment of the initial host target.

Conceptually:

~~~text
Bellatrix
    │
    ▼
Host abstraction
    │
    ▼
RPi3B / Emu68 target
    │
    ├── Emu68 execution
    ├── ARM platform
    ├── MMU
    ├── native IRQ path
    └── host bus integration
~~~

This prevents Emu68 implementation details from spreading throughout Bellatrix Core.

Bellatrix can still be architecturally designed around M68K execution without requiring every target to expose the exact same internal Emu68 implementation.

---

# 33. Host Versus Execution Engine

The Host API and execution-engine API may initially overlap because Emu68 provides many host mechanisms.

They should nevertheless remain conceptually distinct.

~~~text
Bellatrix
   │
   ├── machine policy
   │
   ├── execution coordination
   │
   └── host requirements
          │
          ▼
        Host
          │
          ├── platform
          └── execution engine integration
~~~

A future host may provide a different execution engine.

The architecture should not prevent this unnecessarily.

---

# 34. Initial Host Target

The initial production target remains:

~~~text
Bellatrix
   │
   ▼
RPi3B Host
   │
   ├── Emu68
   ├── BCM2837
   ├── ARM MMU
   ├── Raspberry Pi interrupt controller
   ├── platform timer
   ├── FDT
   └── native device backends
~~~

This target may legitimately contain highly platform-specific code.

The requirement is that the specificity remains contained inside the target implementation.

---

# 35. Future Host Targets

The architecture should allow targets conceptually equivalent to:

~~~text
host/
├── rpi3b/
├── linux/
├── qemu/
├── another-arm-board/
└── future/
~~~

This does not mean every target must be implemented immediately.

The purpose is architectural validation:

> Adding a target should not require redesigning Bellatrix Core.

---

# 36. Host Interface Scope

The minimal Host API should contain only capabilities demonstrated to be required by Bellatrix Core.

Potential categories include:

~~~text
Memory

    map
    unmap
    protect
    query


Execution integration

    install bus/fault handling
    execution coherency


Native interrupts

    expose native interrupt state
    platform interrupt integration


Timing

    monotonic/native counter where required


Execution primitives

    synchronization primitives where required


Platform information

    normalized capabilities


Diagnostics

    logging
~~~

The exact API signatures should not be frozen until actual Bellatrix requirements are inventoried.

---

# 37. Candidate Host Interface

A conceptual interface might resemble:

~~~c
struct bellatrix_host_ops {
    /*
     * Memory
     */
    int (*map)(...);
    int (*unmap)(...);
    int (*protect)(...);

    /*
     * Execution / fault integration
     */
    int (*install_bus_handler)(...);

    /*
     * Native interrupt domain
     */
    unsigned (*get_native_ipl)(...);

    /*
     * Timing
     */
    uint64_t (*get_counter)(...);

    /*
     * Execution coherency
     */
    void (*invalidate_exec)(...);

    /*
     * Diagnostics
     */
    void (*log)(...);
};
~~~

This is not a proposed frozen ABI.

It merely illustrates the intended direction.

The final API should emerge from actual Bellatrix requirements.

---

# 38. Do Not Over-Abstract

Platform independence does not require abstracting every operation.

A useful rule is:

> Abstract semantic dependencies, not every implementation detail.

For example, if Bellatrix genuinely requires:

~~~text
invalidate translated execution for guest range
~~~

that is a meaningful host operation.

An abstraction such as:

~~~text
perform generic architecture-specific cache thing
~~~

is probably too vague.

Likewise:

~~~text
send event to ARM core
~~~

is too implementation-specific if Bellatrix only needs:

~~~text
wake host execution
~~~

The abstraction should express why Bellatrix requires the operation.

---

# 39. Host Threading and Multicore

The host owns execution placement.

Conceptually:

~~~text
Bellatrix work
      │
      ▼
host scheduling policy
      │
      ├── Core 0
      ├── Core 1
      ├── Core 2
      └── Core 3
~~~

Bellatrix components should not generally encode ARM core ownership.

Rigel must not encode ARM core ownership.

This allows a host to choose:

~~~text
single-core execution

multi-core execution

dedicated Rigel worker

shared worker

host OS threads
~~~

without changing Rigel's hardware semantics.

---

# 40. Host Synchronization

If cross-core or cross-thread execution is introduced, synchronization remains a host responsibility.

Rigel may define:

~~~text
instance is non-reentrant

calls must be serialized
~~~

Bellatrix may define:

~~~text
this operation requires Rigel progress

this state must be visible before execution continues
~~~

The host decides how to satisfy those constraints.

For example:

~~~text
mutex

spinlock

queue

WFE / SEV

scheduler primitive
~~~

may all be possible implementations.

These mechanisms must not leak into the Rigel API.

---

# 41. Symmetry with Rigel Host Independence

The Host Abstraction Layer completes the architectural isolation already being established for Rigel.

Without a Bellatrix Host Layer:

~~~text
Rigel
   │
   │ clean host-independent boundary
   ▼
Bellatrix
   │
   │ platform details mixed here
   ▼
RPi3B
~~~

With the Host Layer:

~~~text
Rigel
   │
   ▼
Bellatrix
   │
   ▼
Host API
   │
   ▼
RPi3B
~~~

The two boundaries then have complementary responsibilities.

---

# 42. Rigel Boundary Versus Host Boundary

The distinction can be summarized as:

~~~text
                Bellatrix
              /           \
             /             \
            ▼               ▼
      Rigel API          Host API
          │                  │
          ▼                  ▼
 classic hardware      native platform
    semantics             mechanisms
~~~

The Rigel boundary answers:

> What does classic Amiga hardware do?

The Host boundary answers:

> How does this platform provide the mechanisms Bellatrix needs?

Bellatrix answers:

> How are these components assembled into the machine?

---

# 43. Ownership Matrix

The intended ownership can be summarized as:

| Concern | Rigel | Bellatrix Core | Host | Device Backend |
|---|---|---|---|---|
| Classic registers | Yes | No | No | No |
| INTENA / INTREQ | Yes | No | No | No |
| Classic IPL | Yes | Arbitration only | Delivery mechanism | No |
| Chipset DMA semantics | Yes | Memory integration | Mechanism where required | No |
| Guest memory topology | No | Yes | Mapping mechanism | No |
| Address provider selection | No | Yes | Entry mechanism | No |
| ARM MMU | No | No | Yes | No |
| BCM IRQ controller | No | No | RPi3B target | No |
| ARM core assignment | No | No | Yes | No |
| FDT parsing | No | No | Target-specific | No |
| VC4 programming | No | No | Possibly target support | Video backend |
| Classic video generation | Yes | No | No | No |
| Video presentation policy | No | Yes | Support only | Yes |
| Native storage | No | Machine integration | Support only | Yes |
| USB/Bluetooth transport | No | Adaptation only | Support only | Yes |

The exact placement of individual device infrastructure may evolve.

The ownership principle should remain stable.

---

# 44. Core Invariants

The architecture should enforce the following invariants.

## Bellatrix Core must not depend on BCM-specific headers

~~~text
Bellatrix Core
      ╳
      └── bcm283x.h
~~~

## Bellatrix Core must not know Raspberry Pi peripheral bases

~~~text
0x3Fxxxxxx
~~~

or equivalent platform addresses should not become Bellatrix machine constants unless they are genuinely guest-visible machine architecture.

## Bellatrix Core must not encode ARM core IDs

~~~text
Core 0

Core 1

Core 2

Core 3
~~~

belong to host execution policy.

## Bellatrix Core must not decode board-specific native IRQ controllers

That belongs to the host target.

## Rigel must not depend on the Host API

Rigel depends only on its own host-facing contract implemented by the Bellatrix Rigel adapter.

## Host implementations must not contain classic chipset semantics

The RPi3B host must not implement:

~~~text
DMACON

INTENA

INTREQ

Copper

Blitter

Denise

Paula

CIA
~~~

## Device backends must not redefine machine policy

A VC4 backend presents video.

It does not decide Bellatrix memory topology or Rigel timing semantics.

---

# 45. CONFIG_RIGEL Independence

The Host Abstraction Layer must remain useful with or without Rigel.

Conceptually:

~~~text
CONFIG_RIGEL=n

Bellatrix
   │
   ▼
Host API
   │
   ▼
RPi3B
~~~

and:

~~~text
CONFIG_RIGEL=y

Rigel
   │
   ▼
Bellatrix
   │
   ▼
Host API
   │
   ▼
RPi3B
~~~

Therefore the host abstraction must not become an indirect Rigel dependency.

Likewise, Bellatrix Core must remain valid without the compatibility layer.

---

# 46. Relationship with the 24-Bit Address-Space Policy

The new low-24-bit protection policy provides a useful example of the boundary.

Bellatrix decides:

~~~text
0x000000
    │
    │
    ▼
0xFFFFFF

initially protected
~~~

The host implements the protection mechanism.

Then Bellatrix may classify regions:

~~~text
Chip RAM
    │
    └── map directly

Slow RAM
    │
    └── optionally map directly

MMIO
    │
    └── keep trapped

unmapped holes
    │
    └── preserve unmapped behavior
~~~

The host must not decide which of those regions represent classic Amiga hardware.

That remains Bellatrix/Rigel machine policy.

---

# 47. Relationship with Rigel MMIO

The complete MMIO path becomes:

~~~text
M68K execution
      │
      ▼
execution engine
      │
      ▼
Host bus/fault mechanism
      │
      ▼
Bellatrix address dispatcher
      │
      ▼
Rigel provider selected
      │
      ▼
Bellatrix Rigel adapter
      │
      ▼
canonical Rigel MMIO
      │
      ▼
Rigel chipset semantics
~~~

Each layer has exactly one reason to exist.

~~~text
Execution engine
    executes M68K

Host
    exposes platform mechanism

Bellatrix
    owns machine/provider policy

Rigel adapter
    translates integration contract

Rigel
    owns classic hardware behavior
~~~

---

# 48. Relationship with Rigel DMA

DMA travels through a different path:

~~~text
Rigel chipset
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
Rigel host memory operation
      │
      ▼
Bellatrix guest-memory backend
      │
      ▼
Host memory representation
~~~

If executable-memory coherency is required:

~~~text
DMA write
   │
   ▼
Bellatrix guest memory
   │
   ▼
execution coherency required
   │
   ▼
Host API
   │
   ▼
Emu68-specific invalidation
~~~

Rigel therefore remains completely unaware of Emu68 JIT internals.

---

# 49. Relationship with Video

The complete video relationship should remain:

~~~text
Classic graphics
      │
      ▼
    Rigel
      │
      ▼
host-independent raster/video output
      │
      ▼
Bellatrix compatibility presentation
      │
      ▼
video backend
      │
      ▼
native platform
~~~

Native AROS graphics may independently use:

~~~text
AROS graphics
      │
      ▼
native graphics driver
      │
      ▼
Bellatrix/native video path
      │
      ▼
video backend / GPU
~~~

The two paths may share presentation infrastructure without merging their semantics.

---

# 50. Migration from the Legacy Bellatrix

The legacy Bellatrix implementation should be used as implementation evidence rather than copied structurally.

The migration process should identify:

~~~text
legacy platform-specific code
          │
          ├── actual host mechanism
          │
          ├── Bellatrix machine policy
          │
          ├── Rigel semantics
          │
          └── obsolete coupling
~~~

Each piece should move to its correct owner.

The objective is not:

~~~text
rewrite legacy architecture
~~~

The objective is:

~~~text
extract the boundaries that legacy behavior
demonstrated were actually required
~~~

---

# 51. Legacy Host Mechanisms

Where the legacy implementation already contains mechanisms for:

~~~text
memory mapping

fault handling

host bus integration

interrupt delivery

platform timing

cache coherency

core coordination

video presentation
~~~

those implementations should be evaluated as candidates for the RPi3B host target.

They should not automatically become Bellatrix Core APIs.

The question for each mechanism should be:

> Is this a Bellatrix semantic requirement or merely the way the legacy RPi3B host satisfied that requirement?

If it is the latter, it belongs behind the Host API.

---

# 52. Migration Strategy

Recommended progression:

~~~text
Legacy Bellatrix
       │
       ▼
inventory platform dependencies
       │
       ▼
classify ownership
       │
       ├── Bellatrix machine policy
       ├── Host mechanism
       ├── Device backend
       ├── Rigel behavior
       └── obsolete
       │
       ▼
define minimum Host API
       │
       ▼
implement RPi3B host
       │
       ▼
move Bellatrix Core onto Host API
       │
       ▼
verify RPi3B behavior
       │
       ▼
build second host or test host
       │
       ▼
validate abstraction
~~~

The second host is architecturally important even if it is not production-quality.

It demonstrates whether the abstraction actually represents Bellatrix requirements rather than disguised Raspberry Pi operations.

---

# 53. Phase 0 — Host Dependency Inventory

Before designing the final Host API, inventory every direct dependency Bellatrix currently has on:

~~~text
Emu68 internals

BCM283x

ARM MMU

ARM cache operations

ARM interrupt state

ARM core topology

FDT

VC4

Raspberry Pi firmware

board physical addresses
~~~

Each dependency should be classified as:

~~~text
MACHINE POLICY

HOST MECHANISM

DEVICE BACKEND

EXECUTION ENGINE INTEGRATION

OBSOLETE
~~~

No abstraction should be introduced before the dependency being abstracted is understood.

---

# 54. Phase 1 — Minimal Host API

Define only the mechanisms immediately required by Bellatrix Core.

Initial candidates are likely:

~~~text
memory map/unmap/protect

bus/fault integration

native interrupt integration

execution coherency

platform diagnostics
~~~

Timing and synchronization primitives should be included only where Bellatrix actually requires them.

The Host API should remain deliberately smaller than the RPi3B implementation.

---

# 55. Phase 2 — RPi3B Host

Move Raspberry Pi-specific mechanisms behind:

~~~text
host/rpi3b/
~~~

This includes, where applicable:

~~~text
BCM interrupt-controller handling

ARM MMU implementation

physical platform addresses

FDT processing

native timer access

cache maintenance

Emu68 integration details

ARM core coordination
~~~

The Bellatrix Core build should no longer require these details directly.

---

# 56. Phase 3 — Emu68 Integration

Contain Emu68-specific mechanisms within the host/execution boundary.

For example:

~~~text
Emu68 host bus hook
       │
       ▼
RPi3B Emu68 host integration
       │
       ▼
Bellatrix generic bus entry
~~~

and:

~~~text
Bellatrix compatibility IPL
       │
       ▼
host execution integration
       │
       ▼
Emu68 INTF.IPL
~~~

Likewise:

~~~text
native IRQ
       │
       ▼
Emu68 INTF.ARM
~~~

remains a separate native path.

---

# 57. Phase 4 — Device Backend Separation

Move presentation and physical-device code out of the minimal Host API where appropriate.

Candidates include:

~~~text
video

audio

storage

USB

network

Bluetooth
~~~

These should use dedicated subsystem/backend interfaces.

This prevents the Host API from becoming a monolithic platform abstraction.

---

# 58. Phase 5 — Second-Target Validation

Implement a minimal second target.

It does not initially need to boot the complete production environment.

Its purpose is to answer:

~~~text
Can Bellatrix Core compile without RPi3B headers?

Can machine.c operate without BCM knowledge?

Can address-space policy be exercised independently?

Can the bus dispatcher operate through another host?

Can native IRQ state be represented without BCM assumptions?

Can CONFIG_RIGEL=y operate without RPi-specific Rigel integration?
~~~

Possible validation targets include:

~~~text
Linux test host

QEMU-oriented host

standalone test host
~~~

The simplest target that proves the abstraction should be preferred.

---

# 59. Host API Version 1 Criteria

The Host API should not be considered stable until:

* Bellatrix Core contains no unnecessary RPi3B dependencies;
* Bellatrix Core contains no BCM interrupt-controller logic;
* Bellatrix Core contains no ARM MMU implementation;
* Bellatrix Core contains no fixed ARM core policy;
* guest physical addresses remain distinct from host addresses;
* memory policy remains Bellatrix-owned;
* mapping mechanism remains host-owned;
* native IRQ semantics remain distinct from Rigel IPL;
* Emu68 integration is contained;
* execution coherency is represented semantically;
* Rigel remains independent from the Host API;
* device backends remain separable from the minimal Host API;
* at least one non-RPi3B implementation or test host validates the boundary.

---

# 60. Conformance Tests

The architecture should support tests demonstrating the following.

## Platform-independent Core

Bellatrix Core builds without:

~~~text
BCM headers

Raspberry Pi firmware headers

VC4 headers

board-specific MMU headers
~~~

## Host replacement

Changing:

~~~text
host/rpi3b
~~~

to another valid host implementation does not require changing machine semantics.

## Memory policy isolation

Bellatrix can define guest memory topology without knowing the host mapping mechanism.

## MMIO isolation

Bellatrix can dispatch a trapped M68K access without knowing how the host trapped it.

## Native IRQ isolation

Bellatrix receives native interrupt information without decoding the board-specific interrupt controller itself.

## Rigel isolation

Rigel operates without any direct dependency on:

~~~text
Bellatrix Host API

Emu68

BCM283x

RPi3B
~~~

## Video isolation

Rigel video generation can be tested without VC4.

## Multicore isolation

Changing host execution topology does not require changing Rigel semantics.

---

# 61. Review Checklist

Every new platform-related Bellatrix change should answer:

1. Is this machine policy or host mechanism?
2. Does Bellatrix Core genuinely need to know this detail?
3. Is this operation specific to RPi3B?
4. Is this operation specific to Emu68?
5. Is a BCM register leaking into generic code?
6. Is an ARM physical address leaking into machine semantics?
7. Is a host pointer being confused with a guest physical address?
8. Is Bellatrix encoding a particular MMU implementation?
9. Is Bellatrix encoding a particular interrupt controller?
10. Is Bellatrix encoding ARM core numbers?
11. Is Rigel being given host implementation knowledge?
12. Is host synchronization leaking into Rigel?
13. Is video presentation being confused with classic video generation?
14. Is a native device backend being placed unnecessarily in the Host API?
15. Could another host implement this operation meaningfully?
16. Is the abstraction expressing a semantic requirement or merely renaming an RPi operation?
17. Could the standalone Bellatrix machine tests exercise this without RPi hardware?
18. Does `CONFIG_RIGEL=n` remain valid?
19. Does replacing the host require changes to Rigel?
20. Does replacing Rigel require changes to the host?

If these questions cannot be answered cleanly, the boundary should be reconsidered.

---

# 62. Target Architecture

The intended final architecture is:

~~~text
                           AROS M68K
                               │
                               ▼
                             Emu68
                               │
                               ▼
                         Bellatrix Core
                               │
             ┌─────────────────┼─────────────────┐
             │                 │                 │
          Machine             Bus               IRQ
             │                 │                 │
             └─────────────────┼─────────────────┘
                               │
                    ┌──────────┴──────────┐
                    │                     │
                    ▼                     ▼
              Rigel Adapter          Host Interface
                    │                     │
                    ▼                     ▼
                librigel             RPi3B Host
                    │                     │
       classic Amiga hardware             ├── Emu68 integration
           semantics                      ├── ARM MMU
                                          ├── BCM IRQ
                                          ├── timer
                                          ├── cache
                                          └── platform discovery
~~~

Device presentation remains lateral to the minimal Host API:

~~~text
                         Bellatrix
                             │
              ┌──────────────┼──────────────┐
              │              │              │
           Video          Storage         Input
              │              │              │
              ▼              ▼              ▼
          backend         backend         backend
              │              │              │
              └──────────────┼──────────────┘
                             │
                             ▼
                       native platform
~~~

---

# 63. Complete Boundary Model

The architecture can be understood as three semantic layers.

~~~text
                  Compatibility Semantics
                           │
                           ▼
                        librigel
                           │
                           ▼
                    Rigel Adapter
                           │
                           ▼
                    Bellatrix Core
                           │
                           ▼
                    Host Interface
                           │
                           ▼
                   Platform Mechanism
~~~

Each layer answers a different question.

### Rigel

~~~text
What does classic Amiga hardware do?
~~~

### Bellatrix

~~~text
What machine are we assembling,
and which component owns each region
or architectural function?
~~~

### Host

~~~text
How does this execution environment
provide the mechanisms required
to implement that machine?
~~~

### Device backend

~~~text
How is a native physical or virtual
device actually presented or accessed?
~~~

These questions must remain separate.

---

# 64. Relationship to the Rigel API Convergence Plan

The Rigel API Convergence Plan establishes:

> Rigel must remain host-independent.

This document adds the complementary requirement:

> Bellatrix Core must remain platform-independent above its Host Abstraction Layer.

Together:

~~~text
Rigel
 │
 │ host-independent classic hardware
 ▼
Bellatrix
 │
 │ platform-independent machine policy
 ▼
Host API
 │
 │ target-specific implementation
 ▼
Platform
~~~

This prevents platform-specific concerns from migrating upward merely because they no longer belong inside Rigel.

---

# 65. Architectural Consequence

Without an explicit Host Abstraction Layer, removing platform knowledge from Rigel risks merely relocating that knowledge into Bellatrix Core.

The undesirable transformation would be:

~~~text
Before

Rigel + Bellatrix
       │
       └── mixed platform knowledge
~~~

becoming:

~~~text
After

Rigel
 │
 │ clean
 ▼
Bellatrix
 │
 └── all platform knowledge accumulated here
~~~

The desired transformation is instead:

~~~text
Rigel
 │
 │ classic hardware semantics
 ▼
Bellatrix
 │
 │ machine semantics
 ▼
Host
 │
 │ platform mechanisms
 ▼
Target
~~~

This is the architectural reason the Host Abstraction Layer is necessary.

---

# 66. Final Recommendation

Bellatrix should introduce an explicit Host Abstraction Layer before substantial new RPi3B-specific functionality accumulates in the rewritten Core.

The initial implementation should remain pragmatic.

Do not attempt to design a universal PAL for every hypothetical future platform.

Instead:

~~~text
existing RPi3B requirements
          │
          ▼
identify semantic host requirements
          │
          ▼
define minimal Host API
          │
          ▼
implement RPi3B target
          │
          ▼
validate Bellatrix
          │
          ▼
implement minimal second host
          │
          ▼
refine abstraction
~~~

The key architectural rules are:

> Bellatrix defines machine policy. The host implements platform mechanism.

> The Raspberry Pi 3B is a Bellatrix target, not the definition of Bellatrix.

> Rigel owns classic hardware semantics.

> Bellatrix owns machine composition and cross-domain policy.

> The Host layer owns target-specific execution and platform mechanisms.

> Device backends own native presentation and device-specific implementation.

> FDT is a platform discovery mechanism, not a Bellatrix architectural dependency.

> ARM core placement is a host decision, not a Rigel or Bellatrix hardware semantic.

> Guest physical memory must remain distinct from native host representation.

> Video generation and video presentation must remain separate.

> Native AROS graphics and Rigel compatibility graphics may share a presentation backend without becoming the same graphics architecture.

> Emu68-specific mechanisms should enter Bellatrix through the host/execution boundary rather than becoming generic machine semantics.

The resulting architecture is therefore:

~~~text
                       Classic Amiga
                          semantics
                             │
                             ▼
                           Rigel
                             │
                             ▼
                         Bellatrix
                             │
                     machine semantics
                             │
                             ▼
                         Host / PAL
                             │
                     platform mechanism
                             │
                             ▼
                          Target
~~~

For the initial implementation:

~~~text
Rigel
  │
  ▼
Bellatrix
  │
  ▼
RPi3B / Emu68 Host
  │
  ▼
Raspberry Pi 3B
~~~

For a future target:

~~~text
Rigel
  │
  ▼
Bellatrix
  │
  ▼
Different Host
  │
  ▼
Different Platform
~~~

without requiring either Rigel or Bellatrix machine semantics to be redesigned.

That should be the defining criterion for the Bellatrix Host Abstraction Architecture.
