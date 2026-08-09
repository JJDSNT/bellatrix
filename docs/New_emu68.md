# Bellatrix / Emu68 Minimal Patch Plan

## Host Bus, Amiga IPL, Host IRQ and 24-bit Address-Space Integration

**Status:** Proposed scaffold baseline  
**Goal:** Minimize Bellatrix-specific changes to Emu68  
**Target:** New Bellatrix architecture using Emu68 as the primary CPU engine and Rigel as a secondary Amiga chipset subsystem

---

# 1. Purpose

This document defines the minimal Emu68 patch set required by the new Bellatrix architecture.

The central architectural rule is:

~~~
Emu68 implements:
    CPU execution
    MMU
    exception machinery
    CPU interrupt arbitration

Bellatrix defines:
    the machine
    the ARM platform
    memory policy
    subsystem composition

Rigel implements:
    the classic Amiga chipset subsystem
~~~

An important distinction from PiStorm is that the Amiga chipset is not the machine hosting Emu68.

In Bellatrix:

~~~
Bellatrix / ARM platform
        = primary machine

Emu68 + AROS
        = primary CPU/OS environment

Rigel
        = secondary Amiga hardware subsystem
~~~

Therefore the Bellatrix platform interrupt architecture must not depend on Amiga chipset semantics.

The Emu68 patch set should expose only the generic boundaries required by Bellatrix.

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

---

# 2. Why the Previous Interrupt Architecture Must Change

During the initial AROS port, Bellatrix did not yet have Rigel.

The Emu68 `INTF.ARM` path was inherited from an architecture where the external interrupt lifecycle was coupled to Amiga/PiStorm interrupt semantics.

Conceptually:

~~~
external interrupt
       ↓
INTF.ARM asserted
       ↓
68k level 6
       ↓
Amiga EXTER handling
       ↓
INTREQ acknowledgement
       ↓
INTF.ARM cleared
~~~

This was inappropriate for a standalone AROS port because there was no real Paula/Rigel interrupt controller owning `INTENA`, `INTREQ` and `EXTER`.

Bellatrix therefore temporarily bypassed that lifecycle and injected the desired CPU level through:

~~~
INTF.IPL
~~~

This allowed:

~~~
ARM/platform IRQ
       ↓
Bellatrix decides level 6
       ↓
INTF.IPL = 6
       ↓
68k
~~~

without requiring a synthetic Paula merely to acknowledge Raspberry Pi interrupts.

That solution was appropriate as a transitional mechanism.

It should not define the final architecture.

---

# 3. The New Interrupt Model

With Rigel present, Bellatrix has two genuinely independent interrupt domains.

## Primary Bellatrix platform interrupts

Examples:

~~~
BCM283x timer
SD/eMMC
USB
other ARM peripherals
~~~

These belong to the primary Bellatrix machine.

Their path should be:

~~~
ARM peripheral
      ↓
BCM283x interrupt controller
      ↓
Bellatrix host IRQ
      ↓
INTF.ARM
      ↓
Emu68
      ↓
68k level 6
      ↓
AROS platform IRQ dispatcher
~~~

## Secondary Amiga chipset interrupts

These belong to Rigel.

Their path should be:

~~~
Rigel event
    ↓
INTREQ / INTENA
    ↓
Rigel resolves Amiga IPL
    ↓
INTF.IPL
    ↓
Emu68
    ↓
68k IPL
~~~

Therefore:

~~~
                 Emu68
              ┌────┴────┐
              │         │
          INTF.ARM    INTF.IPL
              ▲         ▲
              │         │
       Bellatrix ARM   Rigel
          platform      Amiga
              │        subsystem
              │
           PRIMARY     SECONDARY
~~~

Neither interrupt domain should depend on the other.

---

# 4. Target Architecture

The intended high-level architecture is:

~~~
                         Emu68
              ┌────────────┼────────────┐
              │            │            │
             MMU        INTF.ARM     INTF.IPL
              │            ▲            ▲
              │            │            │
         Data Abort     host IRQ      Amiga IPL
              │            │            │
              ▼            │            │
          vectors.c        │            │
              │            │            │
       generic bus hook    │            │
              │            │            │
              ▼            │            │
                    Bellatrix
              ┌────────┼────────┐
              │        │        │
          machine   host IRQ   amiga/
              │                 │
              │            ┌────┴────┐
              │            │         │
              │          bus.c     irq.c
              │            │         ▲
              │            │         │
              │            └─ Rigel ─┘
              │
              └── MMU policy
~~~

This produces four distinct boundaries:

~~~
Memory policy:
    machine.c → Emu68 MMU

CPU MMIO:
    Emu68 fault → amiga/bus.c → Rigel

Amiga chipset interrupts:
    Rigel → amiga/irq.c → INTF.IPL

Bellatrix platform interrupts:
    ARM IRQ controller → INTF.ARM
~~~

---

# 5. Bellatrix Source Scaffold

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

Owns only the Amiga chipset IPL boundary.

Conceptually:

~~~
Rigel
  ↓
resolved Amiga IPL
  ↓
amiga_irq_set_ipl()
  ↓
INTF.IPL
  ↓
Emu68
  ↓
68k CPU
~~~

It does not implement:

~~~
ARM platform IRQs
BCM interrupt dispatch
INTENA
INTREQ
~~~

`INTENA` and `INTREQ` belong to Rigel.

ARM platform interrupts belong to the Bellatrix platform.

---

# 6. 24-bit Memory Policy

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
$BFDxxx            CIA-B         → fault
$BFExxx            CIA-A         → fault
...
$C00000-$C7FFFF    Slow RAM      → direct, if later required
...
$DFFxxx            Custom        → fault
...
unknown holes                     → fault
~~~

The exact map is deliberately not assumed in advance.

The initial protected configuration is intended to expose what software actually accesses.

---

# 7. Direct Memory Access

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

# 8. Faulted Bus Access

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

# 9. Unknown Address Handling

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

Bellatrix can record information such as:

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

# 10. The Existing $DFF000 4 KiB Hole

The previous Bellatrix integration created a special faulting page around:

~~~
$DFF000-$DFFFFF
~~~

because the rest of the standalone low address space was directly accessible.

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

# 11. Emu68 Patch Philosophy

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
Zorro
Autoconfig
~~~

should return no integration logic.

Those concepts belong outside Emu68.

---

# 12. Patch 1 — Generic Host Bus Hook

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

Bellatrix supplies the actual host implementation.

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

# 13. What Patch 1 Must Not Do

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

# 14. Remove Transitional `INT_shadow` Register Emulation

The previous standalone Bellatrix patches contained logic for:

~~~
INTENA
INTREQ
INTENAR
INTREQR
~~~

using `INT_shadow`.

That logic existed because standalone Bellatrix did not yet have a chipset implementation owning those registers.

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

becomes:

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

# 15. Patch 2 — Standalone Amiga IPL Input

Suggested patch:

~~~
0002-enable-standalone-amiga-ipl.patch
~~~

## Purpose

Allow standalone Emu68 to consume an externally resolved Amiga IPL through its existing `INTF.IPL` state.

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

# 16. Rigel IPL Must Be Level-Driven

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

Therefore Emu68 must not automatically convert a Rigel IPL into a one-shot event and permanently clear it after entering the exception.

The chipset owns assertion and deassertion of the chipset IPL.

---

# 17. `INTF.ARM` Has a Different Lifecycle Problem

`INTF.ARM` is conceptually the correct channel for Bellatrix platform interrupts.

However, the existing Emu68 lifecycle cannot simply be reused unchanged.

The important distinction is:

~~~
INTF.ARM already exists.

INTF.ARM already produces level 6.

The missing piece is independent ownership
of its assert/deassert lifecycle.
~~~

In the inherited PiStorm-oriented model, clearing the external interrupt is coupled to Amiga interrupt acknowledgement.

Conceptually:

~~~
INTF.ARM = 1
     ↓
level 6
     ↓
Amiga EXTER handling
     ↓
INTREQ acknowledgement
     ↓
INTF.ARM = 0
~~~

That is appropriate when Emu68 participates in an existing Amiga hardware interrupt model.

It is not appropriate when Bellatrix itself owns the primary ARM platform.

Bellatrix requires:

~~~
ARM IRQ arrives
     ↓
assert host interrupt
     ↓
INTF.ARM = 1
     ↓
AROS receives level 6
     ↓
AROS/platform code services BCM source
     ↓
host interrupt no longer pending
     ↓
INTF.ARM = 0
~~~

No Amiga `INTREQ` acknowledgement should be required.

---

# 18. Patch 3 — Independent Host IRQ Assert/Deassert

Suggested patch:

~~~
0003-add-host-irq-assert-deassert.patch
~~~

## Purpose

Expose a clean lifecycle for the existing `INTF.ARM` host interrupt channel.

This patch does **not** create another interrupt mechanism.

It makes the existing mechanism usable independently from PiStorm/Amiga acknowledgement semantics.

The conceptual API can be as small as:

~~~
void M68kSetHostInterrupt(int asserted);
~~~

or explicitly:

~~~
void M68kAssertHostInterrupt(void);
void M68kClearHostInterrupt(void);
~~~

Internally:

~~~
assert
    ↓
INTF.ARM = 1
    ↓
wake execution if necessary


deassert
    ↓
INTF.ARM = 0
~~~

For example:

~~~
void M68kSetHostInterrupt(int asserted)
{
    M68KState *ctx = getCTX();

    ctx->INTF.ARM = asserted ? 1 : 0;

    if (asserted)
        wake_cpu_if_required();
}
~~~

The exact implementation should follow the existing Emu68 synchronization/wakeup conventions rather than introducing a parallel mechanism.

---

# 19. What Patch 3 Must Not Do

Patch 3 must not introduce:

~~~
INTENA
INTREQ
EXTER
INT_shadow
Rigel
Amiga register access
~~~

The lifecycle is owned by the Bellatrix platform.

The conceptual relationship is:

~~~
Bellatrix platform IRQ state
          ↓
M68kSetHostInterrupt()
          ↓
INTF.ARM
          ↓
Emu68 level 6
~~~

The source of truth remains the actual ARM/platform interrupt state.

---

# 20. Why `INTF.ARM` and `INTF.IPL` Must Remain Independent

The two fields describe fundamentally different things.

~~~
INTF.ARM

    Bellatrix primary platform interrupt

    source:
        BCM283x / ARM platform

    CPU presentation:
        fixed level 6


INTF.IPL

    secondary Amiga chipset interrupt level

    source:
        Rigel

    CPU presentation:
        Amiga IPL 1..6
~~~

Therefore:

~~~
ARM platform ─────→ INTF.ARM
                       │
                       ▼
                     level 6


Rigel ─────────────→ INTF.IPL
                       │
                       ▼
                   Amiga IPL
~~~

Neither path should clear, acknowledge or manipulate the other.

---

# 21. No Low-24 Emu68 Patch

There should deliberately be no patch named something like:

~~~
0004-protect-amiga-24bit-space.patch
~~~

The rule:

~~~
low 24-bit starts faulting
~~~

is Bellatrix machine policy.

It belongs in:

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

This preserves:

~~~
mechanism → Emu68

policy → Bellatrix
~~~

---

# 22. `machine.c` Memory Construction

Conceptually:

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

The actual implementation should use the existing Emu68 MMU primitives and existing memory attributes.

Bellatrix should not duplicate the Emu68 page-table implementation.

---

# 23. Progressive Mapping Strategy

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

# 24. Rigel DMA Is Not the CPU Bus

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

# 25. No Zorro or Autoconfig Port

The new patch series must start from the clean Emu68 bare-metal baseline.

The old Bellatrix patches that brought PiStorm Zorro/Autoconfig infrastructure into the standalone build must not be applied.

In particular, the new architecture does not require:

~~~
Zorro II
Zorro III
Autoconfig
$E80000 Autoconfig handling
board enumeration
emu68rom Zorro board
PiStorm expansion infrastructure
~~~

Therefore:

~~~
$E80000
   ↓
no Autoconfig implementation
   ↓
remains part of protected low-24
   ↓
fault
   ↓
Bellatrix bus
   ↓
unhandled unless explicitly implemented later
~~~

The absence of a device in that region is not wasted address space.

It is simply an unmapped part of the Amiga address domain.

---

# 26. Open-Bus and Fault Reentry Safety

The existing Bellatrix/Emu68 work includes protection against fault-handler reentry when an address falls through to a guest alias that has no physical backing.

That protection should be evaluated independently from the architectural patches.

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

If this protection is still required with the new memory policy, it should remain as a safety fix.

It is not part of the Bellatrix bus contract itself.

---

# 27. Debug Instrumentation

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

It should not become part of the permanent Emu68/Bellatrix contract unless there is a clear long-term requirement.

---

# 28. Proposed Emu68 Patch Series

The target functional patch series is now:

~~~
patches/emu68/

0001-add-generic-host-bus-hook.patch

    Purpose:
        expose faulted guest transactions to Bellatrix

    Primary area:
        src/aarch64/vectors.c

    Boundary:
        Emu68 fault
            ↓
        generic callback
            ↓
        Bellatrix amiga/bus.c


0002-enable-standalone-amiga-ipl.patch

    Purpose:
        allow Rigel to publish its resolved Amiga IPL

    Primary area:
        src/ExecutionLoop.c

    Boundary:
        Rigel
            ↓
        Bellatrix amiga/irq.c
            ↓
        INTF.IPL
            ↓
        Emu68


0003-add-host-irq-assert-deassert.patch

    Purpose:
        make the existing INTF.ARM channel independently
        assertable and deassertable by the Bellatrix platform

    Boundary:
        Bellatrix platform IRQ
            ↓
        host IRQ API
            ↓
        INTF.ARM
            ↓
        Emu68 level 6
~~~

These are the **three functional integration patches**.

Additional patches such as:

~~~
fault reentry diagnostics
open-bus safety
debug instrumentation
~~~

should be treated separately as diagnostics or robustness fixes rather than architectural integration boundaries.

---

# 29. Migration from the Old Patch Series

The new series should be rebuilt from the clean Emu68 bare-metal baseline.

Do not layer it on top of the old Bellatrix integration series.

## Old Custom interrupt shadow handling

Remove:

~~~
$DFFxxx
   ↓
vectors.c
   ↓
INT_shadow
~~~

Replace with:

~~~
$DFFxxx
   ↓
fault
   ↓
generic host bus
   ↓
amiga/bus.c
   ↓
Rigel
~~~

## Old host IRQ injection

Remove the transitional use of:

~~~
host IRQ
   ↓
INTF.IPL = 6
~~~

Restore the conceptual ownership:

~~~
host IRQ
   ↓
INTF.ARM
~~~

but add the missing independent lifecycle:

~~~
assert
deassert
~~~

without requiring:

~~~
INTREQ
EXTER
INT_shadow
~~~

## Rigel interrupt path

Use:

~~~
Rigel
   ↓
INTF.IPL
~~~

exclusively for the secondary Amiga chipset subsystem.

## Old Zorro patches

Do not apply them.

Do not port their functionality into the new series.

---

# 30. Final Interrupt Ownership

The final interrupt topology should be:

~~~
                         Emu68
                   interrupt arbiter
                    /             \
                   /               \
             INTF.ARM            INTF.IPL
                 ▲                   ▲
                 │                   │
                 │                   │
        Bellatrix platform         Rigel
                 │                   │
       BCM283x peripherals      INTENA/INTREQ
                 │                   │
             host IRQ            Amiga IRQ
                 │                   │
             PRIMARY             SECONDARY
~~~

The important rule is:

> Rigel does not own Bellatrix interrupts.

And conversely:

> Bellatrix platform IRQ handling does not own Rigel interrupt state.

They meet only at the Emu68 CPU interrupt arbitration boundary.

---

# 31. Final Ownership Model

After the refactoring:

~~~
Emu68
├── M68k execution
├── JIT
├── interpreter
├── MMU implementation
├── exception machinery
├── Data Abort decoding
├── generic host bus callback boundary
├── INTF.ARM host interrupt input
├── INTF.IPL chipset interrupt input
└── CPU interrupt arbitration


Bellatrix machine
├── machine composition
├── low-24 memory policy
├── direct RAM mappings
└── component initialization


Bellatrix platform
├── BCM283x hardware
├── physical interrupt controller
├── host IRQ dispatch
└── INTF.ARM lifecycle


Bellatrix Amiga boundary
├── CPU-visible Amiga bus
└── Rigel IPL bridge


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

# 32. Architectural Acceptance Criteria

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

10. INTF.IPL is level-driven by Rigel.

11. Bellatrix ARM/platform interrupts use INTF.ARM.

12. INTF.ARM can be asserted and deasserted without
    INTREQ, EXTER or INT_shadow.

13. Rigel chipset interrupts use INTF.IPL.

14. INTF.ARM and INTF.IPL remain independent.

15. Rigel DMA does not pass through the CPU fault/bus path.

16. No Zorro or Autoconfig infrastructure is introduced
    into the standalone build.

17. PiStorm-specific interrupt acknowledgement semantics
    are not required by Bellatrix.

18. The new patches are applied to the clean Emu68
    bare-metal baseline, not on top of the old Bellatrix
    Zorro/shadow patch series.
~~~

---

# 33. Guiding Principle

The entire integration should be judged against one rule:

> **Patch Emu68 to expose mechanisms and boundaries; keep Bellatrix machine policy outside Emu68.**

The resulting architecture is:

~~~
                       Bellatrix
                  defines the machine
                         │
             ┌───────────┼───────────┐
             │           │           │
        ARM platform    Emu68       Rigel
          PRIMARY      CPU engine   SECONDARY
             │           │           │
             │      ┌────┴────┐      │
             └─────►│INTF.ARM │      │
                    │         │      │
                    │INTF.IPL │◄─────┘
                    └─────────┘
~~~

Or, in its shortest form:

~~~
Bellatrix host IRQ
    → INTF.ARM

Rigel Amiga IPL
    → INTF.IPL

68k MMIO fault
    → Bellatrix bus
    → Rigel

Bellatrix memory policy
    → Emu68 MMU
~~~

This keeps the primary Bellatrix platform independent from the secondary Amiga chipset subsystem while reusing the mechanisms already present in Emu68 wherever possible.
