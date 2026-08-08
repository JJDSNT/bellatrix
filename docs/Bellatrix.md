Sim. Eu consolidaria tudo em um único documento arquitetural, já incorporando os últimos refinamentos e evitando entrar em detalhes que pertencem à futura especificação de integração.

Bellatrix Architecture

Native M68K Execution Platform with Optional Rigel Compatibility

Status

Proposed Architecture

This document defines the architectural baseline for the new Bellatrix generation.

The central distinction is:

Bellatrix defines the native execution platform. Rigel optionally provides the classic Amiga hardware environment.

Bellatrix must operate independently of Rigel.

Rigel must remain usable independently of Bellatrix.

The integration between them must therefore occur through a narrow, explicit, and stable boundary.

⸻

1. Architectural Goals

Bellatrix is a native M68K platform running on Emu68 and Raspberry Pi hardware.

Its core architecture consists of:

* Emu68 as the M68K execution engine;
* Raspberry Pi as the native hardware platform;
* AROS m68k-emu68 as the operating-system platform port;
* Device Tree based hardware discovery;
* native platform drivers;
* direct M68K interrupt delivery.

Classic Amiga hardware is not part of the Bellatrix Core definition.

When classic Amiga hardware compatibility is required, it is provided by Rigel as an optional component.

The architecture must preserve the following property:

Bellatrix Core
     │
     ├── fully functional by itself
     │
     └── optionally attaches librigel

Rigel must not become a prerequisite for Bellatrix boot, device discovery, interrupt handling, or native platform operation.

⸻

2. Architectural Layers

The overall architecture is:

                       AROS/m68k
                           │
                    m68k-emu68
                    Platform Layer
                           │
                         Emu68
                           │
              ┌────────────┴────────────┐
              │                         │
       Native Platform          Optional Compatibility
              │                         │
      Raspberry Pi HW                librigel
                                        │
                               Classic Amiga HW

The two domains have separate responsibilities.

Bellatrix owns the native execution platform.

Rigel owns the classic Amiga chipset environment.

⸻

3. Bellatrix Core

The Bellatrix Core contains everything required to operate AROS/m68k on the target hardware.

Conceptually:

AROS/m68k
    │
m68k-emu68
    │
Emu68
    │
Raspberry Pi

The Core is responsible for:

* M68K execution;
* platform bootstrap;
* platform description;
* Device Tree delivery;
* native interrupt handling;
* native timers;
* native graphics;
* storage;
* USB;
* native Raspberry Pi peripherals;
* other platform services.

The Core must not depend on:

* Agnus;
* Denise;
* Paula;
* CIAA;
* CIAB;
* Zorro;
* Autoconfig;
* Rigel.

This configuration is the reference Bellatrix platform.

⸻

4. Native Hardware Discovery

Native hardware belongs exclusively to the platform domain.

The preferred discovery path is:

Device Tree
     │
     ▼
m68k-emu68 Platform Layer
     │
     ▼
Native AROS Drivers
     │
     ▼
Raspberry Pi Hardware

Native platform devices must not be artificially represented as Amiga expansion hardware.

For example:

SD
 ↓
Virtual Zorro Board
 ↓
Autoconfig
 ↓
AROS

is not the preferred architecture.

Instead:

FDT
 ↓
AROS Platform Driver
 ↓
SD Controller

This follows a fundamental principle:

M68K defines the processor architecture. It does not imply Amiga hardware architecture.

⸻

5. Native Interrupt Architecture

Native platform interrupts use direct M68K IPL delivery.

Conceptually:

Raspberry Pi Peripheral
          │
          ▼
 BCM Interrupt Controller
          │
          ▼
        Emu68
          │
          ▼
     Direct M68K IPL
          │
          ▼
         AROS

Paula interrupt registers do not participate in this path.

This is the normal Bellatrix interrupt architecture.

Architectural invariant

Native devices MUST NOT generate Paula interrupts.

Native interrupt sources remain entirely within the native platform domain until they are presented to the M68K CPU as an IPL.

⸻

6. Paula Compatibility Path

The Paula interrupt model is not part of Bellatrix native interrupt delivery.

INTENA, INTREQ, INTENAR, and INTREQR belong to the classic Amiga hardware domain.

Their role is to support Rigel.

When Rigel is present:

Rigel Event
     │
     ▼
   INTREQ
     │
     ▼
INTREQ & INTENA
     │
     ▼
Amiga Priority Resolution
     │
     ▼
 Derived M68K IPL
     │
     ▼
    Emu68

Typical interrupt sources include:

* VBlank;
* Copper;
* Blitter;
* Paula;
* CIA.

When Rigel is active, Rigel is authoritative for the classic interrupt state.

The current Emu68 Paula register buffering may serve as a transitional compatibility hook, but it must not become the Bellatrix native interrupt controller.

Architectural invariant

Rigel MUST NOT inject chipset interrupts through the native BCM interrupt domain.

The two interrupt domains converge only at the CPU-visible M68K IPL boundary:

 Native domain                    Compatibility domain
 BCM peripheral                    Rigel event
       │                                │
    BCM IRQ                           INTREQ
       │                                │
     Emu68                      INTREQ & INTENA
       │                                │
 direct IPL                       derived IPL
       │                                │
       └────────────► M68K ◄────────────┘

The mechanism used to arbitrate simultaneously asserted native and compatibility IPL sources is defined by the Bellatrix/Rigel Integration Specification.

⸻

7. Expansion and Autoconfig

Autoconfig is not part of the Bellatrix Core architecture.

Bellatrix does not contain a physical Zorro bus, and native Raspberry Pi hardware does not require Zorro-style discovery.

The Core therefore does not require:

* virtual Expansion Boards;
* Expansion Board registries;
* virtual Expansion ROMs;
* Zorro II configuration state;
* Zorro III configuration state;
* virtual board mapping infrastructure.

The presence of expansion.library inside AROS does not by itself justify implementing Autoconfig in Bellatrix.

expansion.library is an operating-system compatibility concern.

Autoconfig is a machine-level compatibility mechanism.

These must remain separate concepts.

⸻

8. Historical Address Space

Historical addresses must not automatically imply that their historical subsystem exists.

An address access is first an address-space event handled by Emu68.

Conceptually:

M68K Address Access
        │
        ▼
Emu68 Address / Fault Dispatcher
        │
        ▼
Is a provider registered?
       / \
     yes  no
      │    │
      ▼    ▼
 Provider  normal unmapped /
           open-bus behavior

Therefore $E80000 is not intrinsically an Autoconfig subsystem.

Without a compatibility provider:

$E80000
    │
    ▼
Emu68 address handling
    │
    ▼
unmapped / no device

If genuine Autoconfig support is introduced in the future:

$E80000
    │
    ▼
Emu68 address handling
    │
    ▼
Autoconfig provider

The semantics belong to the provider, not to Bellatrix Core.

This avoids creating a hidden or reduced Expansion subsystem simply to preserve historical address behavior.

⸻

9. Rigel

Rigel is an optional hardware compatibility backend implementing the classic Amiga chipset domain.

Rigel owns the behavior of:

* Agnus;
* Denise;
* Paula;
* CIAA;
* CIAB;
* Copper;
* Blitter;
* classic chipset timing;
* classic interrupt state.

Rigel does not define Bellatrix.

Bellatrix does not define the internal implementation of Rigel.

The relationship is:

Bellatrix
    │
    │ narrow compatibility boundary
    ▼
 librigel
    │
    ▼
Classic Amiga Hardware Environment

⸻

10. Classic MMIO

Classic Amiga hardware accesses are routed through the normal Emu68 address/fault mechanism.

Conceptually:

M68K access
     │
     ▼
Emu68 Address Dispatcher
     │
     ▼
Rigel Provider

Typical mappings include:

$DFFxxx ──────► Rigel custom chipset
CIAA space ───► Rigel CIAA
CIAB space ───► Rigel CIAB

Bellatrix must not interpret the internal semantics of:

* Agnus registers;
* Denise registers;
* Paula registers;
* Copper state;
* Blitter state;
* CIA registers.

Those semantics belong exclusively to Rigel.

⸻

11. Rigel Integration Role

Rigel must not be described merely as an MMIO backend.

MMIO is only one integration channel.

Rigel:

* receives register accesses;
* maintains chipset state;
* consumes execution progress;
* advances independently within its own timing domain;
* generates asynchronous events;
* derives classic M68K IPL state.

A more accurate definition is:

Rigel is an optional hardware compatibility backend attached to the Bellatrix/Emu68 platform boundary through MMIO dispatch, execution-progress delivery, and event delivery.

⸻

12. Rigel Public Interface

Bellatrix should interact with Rigel through a small public API.

Conceptually:

rigel_reset(...);
rigel_read8(...);
rigel_read16(...);
rigel_write8(...);
rigel_write16(...);
rigel_step(...);
rigel_next_event(...);
rigel_get_ipl(...);

These names are illustrative only.

The final function names, progress unit, stepping model, and event model belong to the Bellatrix/Rigel Integration Specification.

Bellatrix must not directly manipulate:

* beam position;
* Copper state;
* Paula state;
* CIA timers;
* Blitter state;
* DMA slots;
* internal Rigel scheduling.

Bellatrix sees Rigel as one compatibility component.

⸻

13. Host Services

The API used by Bellatrix to operate Rigel must be distinguished from services Rigel may require from its host.

Conceptually:

                     Rigel Public API
Host ─────────────────────────────► librigel
Host ◄───────────────────────────── librigel
                     Host Ops

Possible host-provided services may include:

* memory reads;
* memory writes;
* logging;
* event notification;
* limited integration hooks.

Conceptually:

struct rigel_host_ops {
    mem_read(...);
    mem_write(...);
    signal_event(...);
    log(...);
};

The exact interface should remain minimal.

Rigel must not call Bellatrix-specific internal functions directly.

⸻

14. librigel Independence

Rigel should be built as an independent library.

Conceptually:

                    ┌─────────────────┐
                    │    librigel     │
                    │                 │
                    │ Agnus           │
                    │ Denise          │
                    │ Paula           │
                    │ CIAA / CIAB     │
                    │ Copper          │
                    │ Blitter         │
                    │ timing          │
                    │ chipset state   │
                    └────────┬────────┘
                             │
                     public interface
                             │
              ┌──────────────┼──────────────┐
              │              │              │
          Bellatrix        Harness       Future Host

Bellatrix is one possible Rigel host.

It is not a conceptual dependency of Rigel.

This allows Rigel to be:

* developed independently;
* unit tested independently;
* validated through a standalone harness;
* reused by future hosts.

No Bellatrix platform implementation should be required to build the Rigel core library.

⸻

15. Timing Ownership

Bellatrix and Rigel have separate timing responsibilities.

Bellatrix understands native execution and platform time:

Bellatrix domain
platform timers
scheduler
CPU progress
native devices

Rigel understands Amiga chipset time:

Rigel domain
beam position
DMA timing
Copper
Blitter
Paula
CIA timers
E-clock

The boundary is:

Bellatrix supplies elapsed execution progress. Rigel defines the meaning of that progress within the Amiga chipset domain.

Conceptually:

Execution progress
       │
       ▼
    librigel
       │
 ┌─────┼───────────────┐
 │     │               │
Beam E-clock          DMA
 │     │               │
Copper CIA       Paula / Blitter

Bellatrix must not translate execution progress into:

* scanlines;
* beam positions;
* E-clock ticks;
* Copper cycles;
* DMA slots.

Those transformations belong to Rigel.

Likewise, MMIO handlers must not fabricate chipset timing from host wall-clock time.

Timing-sensitive register reads must reflect Rigel’s current chipset state.

⸻

16. Event Boundary

Rigel may generate asynchronous events while advancing.

These events remain internal to Rigel until they produce an externally visible result.

For interrupts, the externally visible result is the derived M68K IPL.

Conceptually:

Rigel advances
     │
     ├── Copper progresses
     ├── beam progresses
     ├── CIA timer expires
     ├── Paula event occurs
     └── INTREQ changes
              │
              ▼
         derived IPL
              │
              ▼
            Host

Bellatrix does not need to know which internal chipset component caused the interrupt.

It only needs the resulting CPU-visible state.

⸻

17. Build Model

Bellatrix has one architecture with optional feature sets.

It must not evolve into separate Core and Amiga architectures.

The intended model is:

                     Bellatrix
                         │
               ┌─────────┴─────────┐
               │                   │
        CONFIG_RIGEL=n      CONFIG_RIGEL=y
               │                   │
         Native Core         Native Core
                                  +
                               librigel

Distribution artifacts may use names such as:

bellatrix-core.img
bellatrix-rigel.img

These are two builds of the same platform.

They are not two separate architectures.

⸻

18. Bellatrix Core Build

The Core build contains:

* Emu68;
* AROS m68k-emu68;
* Device Tree;
* native platform drivers;
* native interrupt handling;
* direct IPL;
* native graphics;
* native storage;
* native platform services.

It does not require:

* Rigel;
* classic chipset MMIO;
* Paula interrupt semantics;
* Zorro;
* Autoconfig.

Conceptually:

AROS/m68k
    │
m68k-emu68
    │
Emu68
    │
Native Raspberry Pi Platform

This is the reference Bellatrix build.

⸻

19. Bellatrix Rigel Build

With:

CONFIG_RIGEL=y

the same Bellatrix Core additionally attaches Rigel as a compatibility provider.

Conceptually:

                       AROS/m68k
                           │
                    m68k-emu68
                           │
                         Emu68
                           │
              ┌────────────┴────────────┐
              │                         │
       Native Platform             librigel
              │                         │
      Raspberry Pi HW          Classic Amiga HW

Rigel adds:

* classic chipset MMIO;
* Agnus;
* Denise;
* Paula;
* CIAA;
* CIAB;
* Copper;
* Blitter;
* chipset timing;
* classic interrupt semantics.

It does not replace or redefine the native platform architecture.

⸻

20. Bellatrix Adapter

The Bellatrix-side integration should remain small.

Conceptually:

Bellatrix
   │
   └── rigel_adapter.c
          │
          ├── registers classic MMIO regions
          ├── supplies host memory operations
          ├── supplies execution progress
          ├── observes Rigel events/IPL
          └── participates in CPU-visible IPL arbitration
                   │
                   ▼
               librigel

Chipset implementation details must not leak into this adapter.

If logic for Copper, CIA, Paula, beam timing, Blitter, or other chipset internals begins appearing in Bellatrix, the architectural boundary has been violated.

⸻

21. Harness and Validation

The independent Rigel boundary naturally supports standalone testing.

A Rigel harness should be capable of:

* instantiating librigel;
* providing memory callbacks;
* performing MMIO reads and writes;
* supplying deterministic execution progress;
* observing resulting IPL;
* observing events;
* inspecting chipset state;
* capturing diagnostic logs.

Conceptually:

Test Harness
     │
     ├── MMIO
     ├── execution progress
     ├── memory
     │
     ▼
  librigel
     │
     ├── chipset state
     ├── video state
     ├── events
     └── IPL

The existing Bellatrix development workflow already emphasizes harness-generated logs and direct inspection of AROS source during debugging. That workflow remains appropriate for validating both standalone Rigel behavior and Bellatrix integration.

⸻

22. Architectural Invariants

The following rules are normative.

Core independence

Bellatrix Core MUST boot and operate without Rigel.

Rigel independence

librigel MUST NOT depend on Bellatrix.

Native interrupt ownership

Native devices MUST NOT generate Paula interrupts.

Rigel interrupt ownership

Rigel MUST NOT inject interrupts through the native BCM interrupt domain.

IPL convergence

Native and compatibility interrupt domains may converge only at the CPU-visible M68K IPL boundary.

Native hardware discovery

Native Raspberry Pi hardware MUST use the native platform discovery and driver architecture rather than Autoconfig.

Expansion

Bellatrix Core MUST NOT require a Zorro or Autoconfig subsystem.

Historical address semantics

Historical addresses MUST NOT imply the existence of their historical subsystem. Semantics exist only when a corresponding provider is attached.

MMIO ownership

Bellatrix MUST NOT interpret internal Amiga chipset registers owned by Rigel.

Timing ownership

Bellatrix supplies execution progress. Rigel owns the interpretation of that progress as Amiga chipset time.

Dependency direction

Bellatrix may depend on the public Rigel interface. Rigel MUST NOT depend on Bellatrix internals.

⸻

23. Final Architecture

The resulting architecture is:

                          AROS/m68k
                              │
                       m68k-emu68
                       Platform Layer
                              │
                            Emu68
                              │
               ┌──────────────┴──────────────┐
               │                             │
        Native Platform             Compatibility Boundary
               │                             │
        BCM283x / VC4                    librigel
        SD / USB / timer                     │
               │                 ┌───────────┼───────────┐
               │               Agnus       Denise      Paula
               │                             │
               │                         CIAA / CIAB
               │
        native IRQ → IPL            INTENA/INTREQ
               │                         → IPL
               │                             │
               └──────────────► M68K ◄───────┘

Without Rigel:

AROS → m68k-emu68 → Emu68 → Raspberry Pi

With Rigel:

AROS → m68k-emu68 → Emu68 → Raspberry Pi
                         │
                         └── librigel → Classic Amiga HW

⸻

24. Architectural Definition

The architecture is summarized by two statements:

Bellatrix defines the native M68K execution platform.

Rigel optionally provides a host-independent classic Amiga hardware environment.

Bellatrix is therefore not an Amiga emulator whose core happens to run on Emu68.

It is a native M68K platform that may attach a classic Amiga hardware compatibility backend when required.

This distinction is the architectural baseline for future implementation.

Eu consideraria esta versão pronta para virar o documento-base do projeto. Ela fecha as decisões sobre IPL, Paula, Rigel, Expansion/Autoconfig, timing, ownership e os dois builds sem antecipar detalhes que ainda pertencem à próxima etapa.

O documento seguinte deveria ser separado: Bellatrix/Rigel Integration Specification. Nele entram as decisões que ainda não estão congeladas — arbitragem concreta de IPL, unidade temporal, C ABI, DMA/memory contract, lifecycle, reset e ordem de inicialização.
