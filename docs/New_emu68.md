# Bellatrix / Emu68 Minimal Patch Plan

## Host Bus, Amiga IPL and 24-bit Address-Space Integration

**Status:** Proposed scaffold baseline  
**Goal:** Minimize Bellatrix-specific changes to Emu68  
**Target:** New Bellatrix architecture using Emu68 as the CPU engine and Rigel as the Amiga chipset

---

# 1. Purpose

This document defines the minimal Emu68 patch set required by the new Bellatrix architecture.

The central architectural rule is:

~~~
Emu68 implements the CPU, MMU and exception machinery.

Bellatrix defines and composes the machine.

Rigel implements the Amiga chipset.
~~~

The Emu68 patch set should therefore expose only the generic boundaries required by Bellatrix.

No Emu68 patch should implement:

~~~
Amiga Custom register semantics
CIA register semantics
INTENA
INTREQ
Rigel behavior
Bellatrix address decoding
Zorro
Autoconfig
24-bit machine policy
~~~

The current Bellatrix patch series contains transitional solutions from the stage where standalone Bellatrix did not yet have a real Paula/Rigel implementation.

In particular, the existing patches use `INT_shadow` to emulate parts of the Amiga interrupt controller.

That architecture should not be carried forward.

With Rigel present:

~~~
Rigel owns:
    INTENA
    INTREQ
    Amiga interrupt resolution

Bellatrix owns:
    machine composition
    Amiga bus wiring
    CPU/chipset wiring

Emu68 owns:
    CPU execution
    MMU
    fault machinery
    IPL presentation to the 68k CPU
~~~

---

# 2. Target Architecture

The intended high-level architecture is:

~~~
                         Emu68
                 ┌─────────┴─────────┐
                 │                   │
                MMU            ExecutionLoop
                 │                   ▲
                 │                   │
          Data Abort             Amiga IPL
                 │                   │
                 ▼                   │
             vectors.c               │
                 │                   │
          generic bus hook           │
                 │                   │
                 ▼                   │
              Bellatrix
        ┌────────┴────────┐
        │                 │
 src/amiga/bus.c    src/amiga/irq.c
        │                 ▲
        │                 │
        └────── Rigel ────┘


 src/machine/machine.c
        │
        └── uses the existing Emu68 MMU interface
            to construct the Bellatrix memory policy
~~~

This produces three distinct boundaries:

~~~
Memory policy:
    machine.c → Emu68 MMU

CPU MMIO:
    Emu68 fault → amiga/bus.c → Rigel

Chipset interrupts:
    Rigel → amiga/irq.c → Emu68 IPL
~~~

---

# 3. Bellatrix Source Scaffold

The initial Bellatrix-side scaffold should be:

~~~
src/
├── machine/
│   ├── machine.c
│   └── machine.h
│
└── amiga/
    ├── bus.c
    ├── bus.h
    ├── irq.c
    └── irq.h
~~~

The responsibilities are deliberately narrow.

## `src/machine/machine.c`

Owns machine composition and memory policy.

It decides:

~~~
which address ranges are directly mapped;
which address ranges remain faulting;
which RAM regions exist;
how the low 24-bit address domain is initialized.
~~~

It does not implement MMU page tables itself.

It uses the existing Emu68 MMU interface.

## `src/amiga/bus.c`

Owns the CPU-visible Amiga bus boundary.

It handles transactions that could not be satisfied by direct memory mappings.

Typical destinations are:

~~~
Custom registers → Rigel
CIA-A            → Rigel
CIA-B            → Rigel
unknown range    → unhandled/open-bus policy
~~~

## `src/amiga/irq.c`

Owns the Amiga IPL boundary between Rigel and Emu68.

Conceptually:

~~~
Rigel
  ↓
resolved Amiga IPL
  ↓
amiga_irq_set_ipl()
  ↓
Emu68
  ↓
68k CPU
~~~

It does not implement `INTENA` or `INTREQ`.

Those belong to Rigel.

---

# 4. 24-bit Memory Policy

Bellatrix should initially treat the complete classic 24-bit address domain as inaccessible to direct CPU loads/stores:

~~~
0x00000000 - 0x00FFFFFF

default = fault
~~~

This does not mean that the complete range is MMIO.

It means:

> No low-24 address receives a direct memory mapping until Bellatrix explicitly decides that the range represents normal memory.

Conceptually, `machine.c` starts with:

~~~
low 24-bit
    ↓
FAULT
~~~

and progressively establishes direct mappings.

For example:

~~~
low 24-bit

$000000-$1FFFFF    Chip RAM      → direct
...
$BFDxxx             CIA-B         → fault
$BFExxx             CIA-A         → fault
...
$C00000-$C7FFFF     Slow RAM      → direct, if later required
...
$DFFxxx             Custom        → fault
...
unknown holes                      → fault
~~~

The exact map is deliberately not assumed in advance.

The initial protected configuration is intended to expose what software actually accesses.

---

# 5. Direct Memory Access

A range classified as normal RAM should eventually be mapped directly through the Emu68 MMU.

For example:

~~~
68k
 │
 │ read $00123456
 ▼
translated ARM load
 │
 ▼
MMU
 │
 │ mapping exists
 ▼
Chip RAM backing
~~~

There is:

~~~
no Data Abort
no vectors.c
no amiga_bus_read()
no Rigel register dispatch
~~~

This is the normal fast path.

Therefore:

~~~
RAM → direct MMU mapping
~~~

The fault mechanism is not intended to remain in the path for ordinary RAM.

---

# 6. Faulted Bus Access

MMIO regions remain deliberately inaccessible through normal MMU mappings.

For example:

~~~
move.w #$8200,$DFF096
~~~

becomes:

~~~
68k
 ↓
$DFF096
 ↓
MMU no-access
 ↓
Data Abort
 ↓
Emu68 vectors.c
 ↓
generic host bus hook
 ↓
src/amiga/bus.c
 ↓
Rigel
 ↓
DMACON semantics
~~~

Likewise:

~~~
$BFDxxx / $BFExxx
        ↓
      fault
        ↓
  generic bus hook
        ↓
   amiga/bus.c
        ↓
      Rigel CIA
~~~

The CPU therefore does not receive a direct ARM memory mapping for the chipset registers.

---

# 7. Unknown Address Handling

During the discovery phase, an unknown low-24 access should remain visible.

For example:

~~~
68k reads $A00000
        ↓
MMU fault
        ↓
vectors.c
        ↓
amiga_bus_read()
        ↓
unknown
~~~

Bellatrix can then record information such as:

~~~
operation
address
width
value
68k PC
frequency
~~~

Conceptually:

~~~
BUS24 R16 addr=A00000 pc=00F81234
~~~

The address should not automatically become mapped merely because software accessed it.

Instead, the access is investigated and classified.

Possible classifications include:

~~~
normal RAM
chipset MMIO
legacy hardware
mirror
open bus
invalid access
software probe
~~~

Only normal memory should normally be promoted to a direct MMU mapping.

---

# 8. The Existing $DFF000 4 KiB Hole

The current Bellatrix integration creates a special faulting page around:

~~~
$DFF000-$DFFFFF
~~~

because the rest of the standalone low address space is directly accessible.

The new architecture reverses this policy.

Instead of:

~~~
everything direct
    except
$DFF000 → fault
~~~

Bellatrix starts with:

~~~
everything low-24 → fault
~~~

and explicitly restores direct memory mappings.

Therefore `$DFF000` no longer needs to be a special rule.

It simply remains one of the regions that `machine.c` never promotes to direct RAM.

Conceptually:

~~~
protect low-24

then:

map Chip RAM
map other confirmed RAM

do NOT map:
    $DFFxxx
    $BFDxxx
    $BFExxx
    unknown holes
~~~

---

# 9. Emu68 Patch Philosophy

The goal is not to make Emu68 understand Bellatrix.

The goal is to expose generic integration boundaries.

The acceptance rule is:

> Patch Emu68 to expose boundaries, never to implement the Bellatrix machine.

Ideally, searching the patched Emu68 source for:

~~~
Rigel
Bellatrix
DMACON
INTENA
INTREQ
CIAA
CIAB
~~~

should return no integration logic.

Those concepts belong outside Emu68.

---

# 10. Patch 1 — Generic Host Bus Hook

Suggested patch:

~~~
0001-add-generic-host-bus-hook.patch
~~~

## Purpose

Expose faulted guest memory transactions to the embedding host.

The existing Emu68 fault machinery already reaches the system access path in `vectors.c`.

The patch should add a generic callback around the existing read/write resolution.

Conceptually:

~~~
SYSReadValFromAddr()
        ↓
M68kHostBusRead()
        ↓
handled?
   ├─ yes → return host value
   └─ no  → existing Emu68 behavior
~~~

and:

~~~
SYSWriteValToAddr()
        ↓
M68kHostBusWrite()
        ↓
handled?
   ├─ yes → complete transaction
   └─ no  → existing Emu68 behavior
~~~

Possible API:

~~~
int M68kHostBusRead(
    uint64_t address,
    int size,
    uint64_t *value,
    uint64_t *value2);

int M68kHostBusWrite(
    uint64_t address,
    int size,
    uint64_t value,
    uint64_t value2);
~~~

The exact signature should follow the information already available in the Emu68 fault path.

The important property is the return contract:

~~~
handled
not handled
~~~

A default implementation should preserve normal Emu68 behavior.

For example:

~~~
__attribute__((weak))
int M68kHostBusRead(...)
{
    return 0;
}

__attribute__((weak))
int M68kHostBusWrite(...)
{
    return 0;
}
~~~

Bellatrix then supplies the actual host implementation.

Conceptually:

~~~
Emu68
  ↓
M68kHostBusRead/Write
  ↓
Bellatrix
  ↓
amiga_bus_read/write
  ↓
Rigel
~~~

---

# 11. What Patch 1 Must Not Do

The Emu68 patch must not contain address-specific Amiga logic.

Do not implement:

~~~
if (address == 0xDFF09A)
    ...

if (address == 0xDFF09C)
    ...

if (address == CIAA)
    ...

if (address == CIAB)
    ...
~~~

Do not call Rigel directly.

Do not manipulate Rigel state.

Do not decide that an address is Custom, CIA, Slow RAM or anything else.

The Emu68 side only reports:

> A guest memory transaction faulted. Does the host want to handle it?

The Bellatrix side answers that question.

---

# 12. Remove the Transitional INT_shadow Register Emulation

The existing standalone Bellatrix patches contain logic for:

~~~
INTENA
INTREQ
INTENAR
INTREQR
~~~

using `INT_shadow`.

That logic existed because standalone Bellatrix did not yet have the chipset implementation owning those registers.

With Rigel:

~~~
Rigel owns INTENA
Rigel owns INTREQ
~~~

Therefore the old path:

~~~
$DFF09A
   ↓
vectors.c
   ↓
INT_shadow
~~~

should become:

~~~
$DFF09A
   ↓
fault
   ↓
generic bus hook
   ↓
amiga/bus.c
   ↓
Rigel
~~~

There must not be two independent owners of the same chipset state.

---

# 13. Patch 2 — Standalone Amiga IPL Input

Suggested patch:

~~~
0002-enable-standalone-amiga-ipl.patch
~~~

## Purpose

Allow standalone Emu68 to consume an externally resolved Amiga IPL through its existing IPL state.

The path becomes:

~~~
Rigel
 ↓
INTENA / INTREQ
 ↓
Rigel interrupt resolver
 ↓
current Amiga IPL
 ↓
src/amiga/irq.c
 ↓
INTF.IPL
 ↓
Emu68 ExecutionLoop
 ↓
68k CPU
~~~

Emu68 remains responsible for CPU-level behavior:

~~~
SR interrupt mask comparison
interrupt acceptance
autovector selection
exception entry
~~~

Rigel remains responsible for chipset-level behavior:

~~~
INTENA
INTREQ
interrupt source priority
current chipset IPL
~~~

---

# 14. IPL Must Be Level-Driven

The Rigel IPL is not a one-shot notification.

It represents the current state of the chipset interrupt lines.

For example:

~~~
VBLANK
   ↓
Rigel sets INTREQ.VERTB
   ↓
INTENA allows VERTB
   ↓
Rigel resolves IPL3
   ↓
INTF.IPL = 3
~~~

The level remains asserted.

The guest eventually clears the interrupt:

~~~
guest writes INTREQ
        ↓
bus
        ↓
Rigel
        ↓
INTREQ changes
        ↓
Rigel resolves new IPL
        ↓
amiga_irq_set_ipl(new_level)
        ↓
INTF.IPL = new_level
~~~

If no interrupt remains:

~~~
INTF.IPL = 0
~~~

Therefore Emu68 must not automatically turn a Rigel IPL into a one-shot event and permanently clear it after entering the exception.

The chipset owns assertion and deassertion.

---

# 15. Host IRQs Must Remain Separate

Bellatrix has another interrupt domain:

~~~
Raspberry Pi / ARM platform interrupts
~~~

Examples include:

~~~
hardware timer
SD/eMMC
USB
other BCM283x devices
~~~

These are not Amiga chipset interrupts.

They should therefore remain on the Emu68 host/platform interrupt path.

Conceptually:

~~~
BCM283x interrupt
       ↓
Bellatrix host IRQ path
       ↓
INTF.ARM
       ↓
Emu68
       ↓
fixed level 6
       ↓
AROS platform interrupt dispatcher
       ↓
determines real hardware source
~~~

This must remain independent from:

~~~
Rigel
 ↓
INTF.IPL
~~~

The architecture therefore has:

~~~
                 Emu68
              ┌────┴────┐
              │         │
          INTF.ARM    INTF.IPL
              │         │
             L6      Amiga 1..6
              ▲         ▲
              │         │
        ARM platform   Rigel
~~~

The two paths may eventually produce the same numerical 68k level, but they represent different interrupt domains and must not share ownership state.

---

# 16. Patch 3 — Preserve Host IRQ Separation

Suggested patch:

~~~
0003-keep-host-irqs-on-arm-channel.patch
~~~

Its purpose is not to introduce a new interrupt architecture.

Its purpose is to ensure that the standalone Bellatrix host IRQ path continues to use:

~~~
INTF.ARM
~~~

rather than reusing:

~~~
INTF.IPL
~~~

for platform IRQ delivery.

The rule is:

~~~
INTF.ARM
    = Bellatrix/ARM platform interrupt channel

INTF.IPL
    = Amiga chipset IPL channel
~~~

No `INT_shadow` state should be necessary to bridge those two domains.

---

# 17. No Low-24 Emu68 Patch

There should deliberately be no patch named something like:

~~~
0004-protect-amiga-24bit-space.patch
~~~

The rule:

~~~
low 24-bit starts faulting
~~~

is Bellatrix machine policy.

It therefore belongs in:

~~~
src/machine/machine.c
~~~

not inside Emu68.

Emu68 provides the MMU mechanism.

Bellatrix decides how that mechanism is used.

Conceptually:

~~~
machine.c
   ↓
existing Emu68 mmu_map()
   ↓
construct Bellatrix mappings
~~~

This preserves the architectural direction:

~~~
mechanism → Emu68

policy → Bellatrix
~~~

---

# 18. `machine.c` Memory Construction

Conceptually, `machine.c` performs:

~~~
machine_init()
    │
    ├─ protect complete low-24
    │
    ├─ map known Chip RAM
    │
    ├─ map other confirmed RAM
    │
    └─ leave everything else faulting
~~~

For example:

~~~
static void machine_setup_memory(void)
{
    /*
     * Deny direct access to the complete classic
     * 24-bit address domain.
     */
    protect_low24();

    /*
     * Restore only known direct memory.
     */
    map_chip_ram();

    /*
     * Future mappings are added only when justified.
     *
     * map_slow_ram();
     * map_other_ram();
     */
}
~~~

The actual implementation should use the existing Emu68 MMU primitives and their existing memory attributes.

Bellatrix should not duplicate the Emu68 page-table implementation.

---

# 19. Progressive Mapping Strategy

The initial state is:

~~~
$000000-$FFFFFF
        ↓
      fault
~~~

As the machine is investigated, ranges are classified.

If a range is confirmed as normal memory:

~~~
faulting
   ↓
classification
   ↓
normal RAM
   ↓
machine.c mapping
   ↓
direct access
~~~

For example:

~~~
BEFORE

$C00000
   ↓
fault
   ↓
observe


AFTER deciding Slow RAM is required

$C00000-$C7FFFF
   ↓
direct MMU mapping
   ↓
RAM
~~~

If a range is chipset MMIO:

~~~
faulting
   ↓
classification
   ↓
MMIO
   ↓
remain faulting permanently
~~~

For example:

~~~
$DFFxxx
   ↓
fault
   ↓
bus
   ↓
Rigel
~~~

If a range is genuinely unused:

~~~
faulting
   ↓
classification
   ↓
unused/open bus
   ↓
remain unmapped
~~~

There is no requirement to fill holes in the 24-bit address space.

---

# 20. Rigel DMA Is Not the CPU Bus

CPU access to the chipset and chipset DMA are separate paths.

CPU register access:

~~~
68k
 ↓
MMU fault
 ↓
amiga/bus.c
 ↓
Rigel register
~~~

Rigel DMA:

~~~
Rigel
 ├─ Copper
 ├─ Blitter
 ├─ bitplanes
 ├─ sprites
 └─ Paula
      ↓
  Chip RAM backing
~~~

Rigel DMA should not generate CPU MMU faults and should not route through `amiga/bus.c`.

Likewise, Bellatrix platform `dma.resource` is a different concern and should not be used to model classic Amiga chipset DMA.

---

# 21. Open-Bus and Fault Reentry Safety

The existing Bellatrix/Emu68 work includes protection against fault-handler reentry when an address falls through to a guest alias that has no physical backing.

That protection should be preserved.

Conceptually:

~~~
guest access
     ↓
first fault
     ↓
bus hook
     ↓
not handled
     ↓
existing Emu68 fallback
     ↓
backing exists?
   /             \
 yes              no
  │                │
 RAM         nested external abort
                   │
                   ▼
                open bus
~~~

This is a safety mechanism beneath the Bellatrix bus boundary.

It should not be replaced with a second Bellatrix memory-map database.

---

# 22. Debug Instrumentation

During the initial low-24 discovery phase, additional instrumentation may be useful.

For example:

~~~
BUS24 R16 addr=A00000 pc=00F81234
BUS24 W16 addr=DFF096 pc=00F81302 value=8200
BUS24 R8  addr=BFE001 pc=00123456
~~~

Useful fields include:

~~~
address
read/write
access width
written value
68k PC
hit count
~~~

This instrumentation is diagnostic.

It should not become part of the permanent architectural contract between Emu68 and Bellatrix unless there is a clear long-term use for it.

---

# 23. Proposed Emu68 Patch Series

The target patch series should conceptually be:

~~~
patches/emu68/

0001-add-generic-host-bus-hook.patch

    Files:
        src/aarch64/vectors.c
        possibly a small public host-hook header/source

    Purpose:
        expose faulted guest transactions to the host

    Must not contain:
        Bellatrix knowledge
        Rigel knowledge
        Amiga register decoding


0002-enable-standalone-amiga-ipl.patch

    Files:
        src/ExecutionLoop.c
        possibly public CPU API/header

    Purpose:
        allow externally resolved INTF.IPL in standalone mode
        preserve level-triggered semantics


0003-keep-host-irqs-on-arm-channel.patch

    Files:
        only the existing host IRQ integration points as required

    Purpose:
        platform IRQ → INTF.ARM
        Rigel IPL    → INTF.IPL

        keep the two domains independent


0004-report-first-syshandler-reentry.patch

    Status:
        diagnostic / optional

    Purpose:
        help diagnose recursive fault handling


0005-open-bus-for-unmapped-guest-addresses.patch

    Status:
        safety mechanism

    Purpose:
        prevent recursive external aborts from destroying
        the ARM stack when no backing exists
~~~

There is deliberately no:

~~~
low24.patch
rigel.patch
custom-register.patch
cia.patch
zorro.patch
autoconfig.patch
~~~

---

# 24. Migration from the Current Patch Series

The existing patches should be converged rather than continuously extended.

## Current shadow-register patch

Current concept:

~~~
$DFF09A/$DFF09C
        ↓
vectors.c
        ↓
INT_shadow
~~~

Replace with:

~~~
$DFF09A/$DFF09C
        ↓
fault
        ↓
generic host bus hook
        ↓
amiga/bus.c
        ↓
Rigel
~~~

## Current standalone IPL patch

Keep the concept:

~~~
external resolved IPL
        ↓
INTF.IPL
        ↓
Emu68
~~~

but ensure that IPL behaves as a level rather than an automatically consumed pulse.

## Current host IRQ → IPL patch

Replace:

~~~
host IRQ
   ↓
INTF.IPL = 6
~~~

with:

~~~
host IRQ
   ↓
INTF.ARM
   ↓
Emu68 level 6
~~~

Reserve:

~~~
INTF.IPL
~~~

for Rigel.

## Reentry/open-bus patches

Keep them unless the underlying issue is solved more cleanly in the new integration.

---

# 25. Final Ownership Model

After the refactoring:

~~~
Emu68
├── M68k execution
├── JIT
├── interpreter
├── MMU implementation
├── exception machinery
├── Data Abort decoding
├── host bus callback boundary
└── CPU interrupt arbitration


Bellatrix machine
├── machine composition
├── low-24 memory policy
├── direct RAM mappings
└── component initialization


Bellatrix Amiga boundary
├── CPU-visible Amiga bus
└── Amiga IPL bridge


Rigel
├── Custom registers
├── CIA-A
├── CIA-B
├── INTENA
├── INTREQ
├── chipset interrupt resolution
├── chipset timing
└── chipset DMA
~~~

---

# 26. Architectural Acceptance Criteria

The scaffold is considered correctly implemented when:

~~~
1. Emu68 contains no Rigel-specific code.

2. Emu68 vectors.c contains no Custom/CIA register
   implementation.

3. INT_shadow is not the authority for Rigel INTENA/INTREQ.

4. CPU accesses to Custom/CIA remain fault-driven.

5. CPU accesses to mapped RAM are direct and do not fault.

6. Bellatrix can initially protect the complete low-24 domain.

7. Bellatrix can progressively map individual RAM ranges
   without modifying Emu68.

8. Unknown low-24 accesses remain observable.

9. Rigel can assert and deassert the Amiga IPL.

10. Emu68 does not automatically consume Rigel IPL as a
    one-shot event.

11. ARM/platform interrupts use INTF.ARM.

12. Rigel chipset interrupts use INTF.IPL.

13. Rigel DMA does not pass through the CPU fault/bus path.

14. No Zorro or Autoconfig infrastructure is required by
    the core machine scaffold.

15. PiStorm-specific behavior remains isolated from the
    standalone Bellatrix architecture.
~~~

---

# 27. Guiding Principle

The entire integration should be judged against one rule:

> **Patch Emu68 to expose mechanisms and boundaries; keep Bellatrix machine policy outside Emu68.**

This produces the desired relationship:

~~~
              Bellatrix
           defines the machine
                  │
        ┌─────────┴─────────┐
        │                   │
      Emu68                Rigel
    CPU engine             chipset
        │                   │
        └──── src/amiga ────┘
               glue
~~~

The resulting Emu68 changes should therefore become smaller as the Bellatrix architecture becomes more complete, not larger.
