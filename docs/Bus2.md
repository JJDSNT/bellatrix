Bellatrix Bus Integration Architecture

Reintroducing a Minimal Machine Transaction Boundary from the Legacy Emu68 Integration

Status: Proposed Architectural Direction
Scope: Bellatrix target / Emu68 integration
Historical reference: Bellatrix legacy branch
Related: Bellatrix.md, Emu68_target_boundary.md, Expansion.md, Rigel_integration.md

⸻

1. Purpose

This document defines the recommended direction for introducing a Bellatrix machine transaction boundary between Emu68 and Bellatrix.

The architecture is based on a useful property already demonstrated by the Bellatrix legacy implementation:

Emu68 fault path
      │
      ▼
Bellatrix-specific hook
      │
      ▼
Bellatrix machine/bus boundary
      │
      ▼
machine semantics

The legacy implementation should therefore be treated as an implementation reference for locating and reconstructing the correct Emu68 hook.

The objective is not to restore the complete historical Bellatrix bus architecture.

The objective is to recover its most important architectural property:

An M68K access intentionally trapped by the Bellatrix target crosses one explicit Bellatrix boundary before being interpreted as machine hardware.

The resulting boundary will be referred to in this document as the Bellatrix Bus.

⸻

2. Architectural Decision

Bellatrix is a first-class Emu68 target.

The Bellatrix target owns the Bellatrix machine definition.

Within that target, the Bellatrix Bus is the boundary through which M68K-visible hardware transactions that are not directly represented by MMU mappings enter the Bellatrix machine.

Conceptually:

                     Emu68
                       │
                execution core
                       │
                 MMU + vectors
                       │
                       ▼
             Bellatrix target hook
                       │
                       ▼
                 Bellatrix Bus
                       │
          ┌────────────┼────────────┐
          │            │            │
      Expansion     optional      Bellatrix
                    Rigel         native
                                 semantics

The Bellatrix Bus is subordinate to the Bellatrix target.

It is not a generic Emu68 provider framework.

⸻

3. Why a Bus Boundary Is Useful

Some M68K addresses represent ordinary memory.

Others represent hardware transactions.

For ordinary memory, direct MMU mapping is desirable:

M68K access
    │
    ▼
Emu68 MMU
    │
    ▼
direct translation
    │
    ▼
guest memory

For hardware regions, direct memory mapping may be inappropriate because the access itself has semantics.

For those regions:

M68K access
    │
    ▼
Emu68 MMU
    │
    ▼
intentional translation fault
    │
    ▼
Data Abort
    │
    ▼
Emu68 fault reconstruction
    │
    ▼
Bellatrix Bus
    │
    ▼
machine semantics

The Bellatrix Bus therefore represents a machine transaction boundary, not an alternative memory allocator or general-purpose software bus.

⸻

4. Legacy Bellatrix as the Implementation Reference

The Bellatrix legacy branch already implemented the important part of this relationship.

Conceptually:

M68K JIT access
      │
      ▼
address intentionally not mapped
      │
      ▼
Data Abort
      │
      ▼
vectors.c
      │
      ▼
Bellatrix vectors hook
      │
      ▼
bellatrix_bus_access()
      │
      ▼
Bellatrix machine decode

The legacy implementation is therefore valuable for determining:

* where the Bellatrix hook belongs in the Emu68 fault path;
* what information is already available when the hook is reached;
* how read and write accesses are reconstructed;
* how access width is determined;
* how the faulting M68K address is recovered;
* how read results are returned to translated execution;
* how writes are forwarded;
* how the Bellatrix path coexists with existing Emu68 exception handling.

The legacy branch should be studied before redesigning this path.

⸻

5. What Should Be Reused from Legacy

The primary reusable architectural idea is:

Emu68
   │
   ▼
one Bellatrix hook
   │
   ▼
Bellatrix machine boundary

In particular, the new implementation should investigate and reuse where appropriate the concepts represented historically by:

vectors.c
Bellatrix vectors integration
bellatrix_bus_access()
Bellatrix memory/machine decode

The legacy implementation provides evidence that the Emu68 fault mechanism can successfully act as the transport for Bellatrix hardware accesses.

⸻

6. What Should Not Be Restored Automatically

The legacy architecture must not be copied mechanically.

In particular, this direction does not imply restoration of:

* the complete historical VirtualBus;
* old multicore synchronization;
* request/response queues;
* epochs;
* WFE/SEV protocols;
* historical Core ownership;
* old timing synchronization;
* obsolete device abstractions;
* obsolete machine decode structures.

Those mechanisms addressed architectural requirements of the older Bellatrix implementation.

The new bus should initially contain only what is necessary to express a synchronous M68K-visible machine transaction.

⸻

7. The Bus Is Not the Aggregating Architecture

The Bellatrix Bus must not become the top-level architecture.

The hierarchy is:

Bellatrix target
      │
      ▼
Bellatrix Bus
      │
      ├── Expansion
      ├── optional Rigel
      └── other Bellatrix machine domains

Not:

Generic Bus
    │
    ├── PiStorm
    ├── Bellatrix
    ├── Rigel
    └── Expansion

The target remains the architectural aggregation point.

The bus is merely one mechanism used by the Bellatrix target.

⸻

8. Relationship to the Three Emu68 Targets

The broader Emu68 architecture contains three distinct target environments:

Emu68
  │
  ├── PiStorm
  │
  ├── Bare-metal non-PiStorm
  │
  └── Bellatrix

The Bellatrix Bus belongs only to:

Emu68
  │
  └── Bellatrix
          │
          ▼
     Bellatrix Bus

PiStorm may continue using its own machine-access mechanisms.

Bare-metal non-PiStorm may continue using its existing memory model.

The Bellatrix Bus MUST NOT become a mandatory generic Emu68 abstraction merely because Bellatrix requires it.

⸻

9. MMU and Bus Must Be Designed Together

The Bellatrix MMU policy determines which M68K accesses reach the Bellatrix Bus.

Therefore:

Bellatrix MMU policy and Bellatrix Bus decode are two parts of the same machine definition.

Conceptually:

                Bellatrix target
                       │
             ┌─────────┴─────────┐
             │                   │
        MMU policy           Bus semantics
             │                   │
     what should trap       what trapped
                            accesses mean

For every intentionally trapped region:

Bellatrix MMU
      │
      └── no direct translation
                │
                ▼
             fault
                │
                ▼
         Bellatrix Bus
                │
                └── defined machine semantics

⸻

10. Mapping Invariant

A hardware range intended for Bellatrix Bus handling MUST NOT simultaneously have a normal direct MMU mapping that bypasses the bus.

For example, if:

$00E80000-$00E8FFFF

represents Expansion/Autoconfig, the following would be invalid:

MMU
    $E80000 → ordinary RAM mapping
Bellatrix Bus
    $E80000 → Expansion

because normal execution would never reach the Expansion implementation.

The correct relationship is:

MMU
    $E80000 → intentionally trapped
Bellatrix Bus
    $E80000 → Expansion

⸻

11. Intentionally Trapped Is Not Unmapped Hardware

The implementation and documentation should preserve an important distinction:

no direct MMU mapping
        ≠
no architectural meaning

An address can intentionally lack ARM translation while remaining valid M68K hardware space.

Therefore:

$E80000

may be:

ARM MMU:
    no translation
Bellatrix machine:
    Expansion / Autoconfig

Similarly, classic chipset ranges may be intentionally trapped while remaining valid machine hardware.

The preferred terminology is therefore:

intentionally trapped

rather than simply:

unmapped

when the address has defined Bellatrix machine semantics.

⸻

12. Minimal Bellatrix Bus Transaction

The initial Bellatrix Bus should be synchronous and deliberately small.

Conceptually, a transaction needs to preserve at least:

M68K address
access direction
access width
write value where applicable
read result where applicable
transaction result

A possible conceptual representation is:

struct bellatrix_bus_transaction {
    uint32_t address;
    enum bellatrix_bus_direction direction;
    enum bellatrix_bus_width width;
    uint32_t value;
};

and conceptually:

bellatrix_bus_result_t
bellatrix_bus_access(
    struct bellatrix_bus *bus,
    struct bellatrix_bus_transaction *transaction);

These signatures are illustrative.

The exact C representation should be selected only after the legacy fault path has been studied.

⸻

13. Preserve the M68K Transaction

The Bellatrix Bus receives an M68K-visible hardware transaction.

It should therefore preserve:

* M68K address;
* width;
* read/write direction;
* logical value;
* occurrence;
* ordering;
* side effects.

Conceptually:

M68K execution
      │
      ▼
WRITE.W $8200,$DFF096
      │
      ▼
Emu68
      │
      ▼
Bellatrix Bus
address   = $DFF096
width     = 16
direction = write
value     = $8200

Host ARM byte ordering must not redefine the logical transaction.

⸻

14. Emu68 Responsibility

Emu68 remains responsible for determining what access caused the fault.

Conceptually:

Data Abort
    │
    ▼
Emu68 exception machinery
    │
    ├── inspect exception state
    ├── identify faulting address
    ├── determine read/write
    ├── determine access width
    ├── recover write value
    └── provide mechanism for read result
             │
             ▼
       Bellatrix target hook

The Bellatrix Bus should not duplicate ARM exception decoding.

The division is:

Emu68:
    what M68K access happened?
Bellatrix:
    what does that access mean?

⸻

15. vectors.c Responsibility

vectors.c should remain primarily part of generic Emu68 exception machinery.

It should not gradually accumulate logic such as:

if address == Expansion...
if address == custom register...
if address == CIA...
if address == Bellatrix device...

Instead, once the access has been reconstructed sufficiently:

vectors.c
    │
    ▼
Bellatrix target hook
    │
    ▼
Bellatrix Bus

The Bellatrix target then owns machine interpretation.

⸻

16. Recommended Emu68 Hook

The precise hook must be derived from the current and legacy Emu68 implementation.

Architecturally, however, it should occur after:

ARM exception captured
faulting address identified
access direction identified
access width identified
write value recoverable

and before:

target-specific machine semantics

Conceptually:

vectors.c
    │
    ▼
generic fault reconstruction
    │
    ▼
target access hook
    │
    ├── PiStorm path
    ├── bare-metal behavior
    └── Bellatrix path
              │
              ▼
        Bellatrix Bus

⸻

17. Do Not Put Rigel in vectors.c

Rigel MUST NOT become directly known by the generic Emu68 exception implementation.

The following relationship should be avoided:

vectors.c
    │
    ├── if custom register → Rigel
    ├── if CIA → Rigel
    └── ...

The desired relationship is:

vectors.c
    │
    ▼
Bellatrix target
    │
    ▼
Bellatrix Bus
    │
    ▼
Bellatrix Rigel adapter
    │
    ▼
public Rigel API
    │
    ▼
librigel

This preserves both Emu68 independence and Rigel independence.

⸻

18. Do Not Put Expansion in vectors.c

The same rule applies to Expansion.

Avoid:

vectors.c
    │
    └── if $E80000 → Autoconfig

Prefer:

vectors.c
    │
    ▼
Bellatrix target hook
    │
    ▼
Bellatrix Bus
    │
    ▼
Expansion

The generic exception mechanism should not know classic Expansion semantics.

⸻

19. Bellatrix Bus Decode

The Bellatrix Bus may initially use explicit machine decode.

Conceptually:

bellatrix_bus_access()
        │
        ├── Expansion range?
        │       │
        │       └── Expansion
        │
        ├── classic chipset range?
        │       │
        │       └── Rigel adapter
        │
        ├── Bellatrix native range?
        │       │
        │       └── native handling
        │
        └── otherwise
                │
                └── defined target behavior

There is no requirement to introduce dynamic provider registration.

A static machine definition is appropriate while the address domains are known.

⸻

20. Expansion Integration

Expansion should be one of the first users of the new Bellatrix Bus.

Conceptually:

AROS expansion.library
        │
        ▼
M68K access $E80000
        │
        ▼
Emu68 MMU
        │
        ▼
intentional fault
        │
        ▼
vectors.c
        │
        ▼
Bellatrix target hook
        │
        ▼
Bellatrix Bus
        │
        ▼
Expansion / Autoconfig

This allows AROS to observe a real machine-level Expansion address rather than requiring an independent Emu68-specific software model.

⸻

21. Chipset Integration

The same boundary can later carry classic chipset transactions.

Conceptually:

M68K access
    │
    ▼
$DFFxxx / CIA range
    │
    ▼
Bellatrix MMU
    │
    ▼
intentional fault
    │
    ▼
Emu68
    │
    ▼
Bellatrix Bus
    │
    ▼
Rigel adapter
    │
    ▼
librigel

Bellatrix determines that the transaction belongs to its optional classic compatibility hardware domain.

Rigel determines what the address means inside that domain.

⸻

22. Rigel Remains Optional

The Bellatrix Bus must exist independently from Rigel.

A Bellatrix build without Rigel may still contain:

Bellatrix target
      │
      ▼
Bellatrix Bus
      │
      ├── Expansion
      └── native machine semantics

A build with Rigel may contain:

Bellatrix target
      │
      ▼
Bellatrix Bus
      │
      ├── Expansion
      ├── native machine semantics
      └── Rigel

Therefore:

Bellatrix Bus
      │
      ╳
      └── does not depend architecturally on Rigel

Rigel is one optional destination of Bellatrix machine transactions.

⸻

23. CONFIG_RIGEL=n

The architecture must remain valid when Rigel is disabled.

Conceptually:

CONFIG_RIGEL=n
$E80000
    │
    ▼
Expansion
    │
    ▼
works
CONFIG_RIGEL=y
$E80000
    │
    ▼
Expansion
    │
    ▼
works
$DFFxxx
    │
    ▼
Rigel

Expansion and Bellatrix Core MUST NOT require Rigel merely because they share the Bellatrix Bus.

⸻

24. Relationship to Rigel’s Canonical MMIO Boundary

The Bellatrix Bus and Rigel MMIO API serve different scopes.

Bellatrix Bus
      │
      │ Bellatrix machine transaction routing
      ▼
Rigel adapter
      │
      │ Rigel compatibility-domain transaction
      ▼
Rigel MMIO API
      │
      ▼
classic chipset semantics

The Bellatrix Bus determines that an address belongs to the optional Rigel compatibility domain.

Rigel determines what the address means within that domain.

For example:

Bellatrix Bus:
$DFF096
    → Rigel compatibility domain
Rigel:
$DFF096
    → custom register semantics

Bellatrix MUST NOT need to know that $DFF096 represents DMACON.

⸻

25. Bus Versus Generic Provider Framework

This architecture deliberately avoids requiring:

provider objects
provider registration
provider priorities
dynamic provider lists
generic device lifecycle
generic provider callbacks

The initial Bellatrix Bus can use explicit machine decode.

Conceptually:

address
   │
   ▼
Bellatrix machine decode
   │
   ├── Expansion
   ├── Rigel
   └── native

If future requirements demonstrate a real need for a more dynamic abstraction, it can be introduced later.

It should not be introduced speculatively.

⸻

26. Bus Versus Memory Map

The Bellatrix Bus must not become a second independent memory map.

There should be one coherent Bellatrix machine definition.

Conceptually:

Bellatrix machine definition
          │
          ├── direct memory ranges
          │       │
          │       ▼
          │      MMU
          │
          └── hardware transaction ranges
                  │
                  ▼
            Bellatrix Bus

The MMU and bus decode are therefore derived from the same target architecture.

They must not evolve independently.

⸻

27. Possible Bellatrix Address Classification

The eventual Bellatrix machine definition may conceptually classify addresses as:

DIRECT
    ordinary memory represented by MMU mapping
BUS
    architecturally valid hardware transaction
    intentionally trapped and forwarded
INVALID
    no Bellatrix machine semantics

For example:

guest RAM
    → DIRECT
Fast RAM
    → DIRECT
ROM
    → DIRECT
Expansion / Autoconfig
    → BUS
classic chipset MMIO when enabled
    → BUS
true address hole
    → INVALID

The exact map belongs to the Bellatrix machine specification.

⸻

28. Bus Result Semantics

The Bellatrix Bus must distinguish:

transaction handled with defined machine behavior
from
no Bellatrix machine semantics
from
internal integration failure

The exact representation remains to be defined.

Conceptually:

Bellatrix Bus result
        │
        ├── handled
        │
        ├── architecturally absent / target-defined response
        │
        └── integration failure

A valid hardware behavior must not accidentally become a host error.

Likewise, an internal implementation failure must not silently become invented hardware behavior.

⸻

29. Synchronous First

The first implementation should be synchronous.

Conceptually:

Emu68 fault
     │
     ▼
Bellatrix Bus transaction
     │
     ▼
machine component
     │
     ▼
result
     │
     ▼
Emu68 resumes

Do not initially introduce:

cross-core request queues
asynchronous completion
epochs
WFE/SEV synchronization
bus worker threads

unless correctness proves they are necessary.

This keeps the architectural boundary independent from execution topology.

⸻

30. Multicore Is a Later Concern

The existence of a synchronous Bellatrix Bus API does not prevent later cross-core execution.

Future implementation could internally become:

bellatrix_bus_access()
        │
        ▼
synchronous architectural boundary
        │
        ▼
internal transport
        │
        ├── same-core call
        │
        └── cross-core request

The caller should not need to know.

Therefore:

Execution topology must not be encoded into the Bellatrix Bus transaction contract.

⸻

31. Timing Is Not Owned by the Bus

The Bellatrix Bus routes hardware transactions.

It must not become the authoritative chipset clock.

For Rigel:

Bellatrix Bus
      │
      ▼
Rigel MMIO

is separate from:

Bellatrix execution progress
      │
      ▼
Rigel temporal API
      │
      ▼
authoritative chipset timeline

The bus must not reproduce Rigel timing semantics.

⸻

32. Interrupts Are Not Owned by the Bus

Likewise, interrupt ownership remains separate.

Conceptually:

Bellatrix Bus
    │
    └── carries M68K hardware transactions
Rigel
    │
    └── produces Rigel IPL
Expansion/native devices
    │
    └── produce their defined interrupt state
Bellatrix
    │
    ▼
IPL arbitration

The bus should not become an interrupt controller merely because hardware accessed through it may generate interrupts.

⸻

33. DMA Is Not the Reverse Bellatrix Bus

Chipset DMA should not automatically be represented as Bellatrix Bus transactions.

For example:

Rigel
  │
  ▼
chipset-generated DMA address
  │
  ▼
Rigel address semantics
  │
  ▼
guest physical address
  │
  ▼
host memory backend

This is different from:

M68K CPU
  │
  ▼
hardware MMIO
  │
  ▼
Bellatrix Bus

The two directions must remain semantically distinct.

⸻

34. Recommended Initial Source Boundary

The exact filesystem layout is not normative.

A minimal implementation could conceptually resemble:

Emu68
│
├── vectors.c
├── mmu.c
└── target hook
        │
        ▼
Bellatrix
│
├── target/
│   ├── memory.c
│   └── bus.c
│
├── expansion/
│   └── ...
│
└── rigel/
    └── adapter.c

The important dependency direction is:

Emu68 generic machinery
        │
        ▼
Bellatrix target hook
        │
        ▼
Bellatrix Bus
        │
        ├── Expansion
        └── Rigel adapter

not the exact filenames.

⸻

35. Recommended Investigation Before Implementation

Before writing the new bus, inspect the legacy implementation and answer concretely:

1. Where does the legacy Bellatrix hook enter vectors.c?
2. At that point, what information about the faulting access is already known?
3. How is the M68K-visible address reconstructed?
4. How is read versus write determined?
5. How is width determined?
6. How is a write value extracted?
7. How is a read result returned?
8. Which registers or execution state are modified before returning?
9. Which logic is generic Emu68 machinery?
10. Which logic exists only for Bellatrix?
11. How did vectors.inc reach bellatrix_bus_access()?
12. How did the legacy bus classify the address?
13. Which parts of that classification remain architecturally valid?
14. Which parts belong only to obsolete Bellatrix architecture?
15. How was the MMU configured so these addresses actually reached the hook?

These answers should drive the new implementation.

⸻

36. Refactoring Sequence

The recommended implementation sequence is:

1. Study legacy vectors.c integration
        │
        ▼
2. Study legacy Bellatrix vectors hook
        │
        ▼
3. Study bellatrix_bus_access()
        │
        ▼
4. Study legacy memory decode
        │
        ▼
5. Study corresponding legacy MMU mappings
        │
        ▼
6. Compare with current Emu68 vectors.c
        │
        ▼
7. Compare with current Emu68 MMU setup
        │
        ▼
8. Identify minimal generic fault reconstruction point
        │
        ▼
9. Introduce Bellatrix target hook
        │
        ▼
10. Introduce minimal synchronous Bellatrix Bus
        │
        ▼
11. Reproduce existing Bellatrix behavior
        │
        ▼
12. Validate PiStorm remains unchanged
        │
        ▼
13. Validate bare-metal remains unchanged
        │
        ▼
14. Add Expansion semantics
        │
        ▼
15. Validate standard AROS Expansion path
        │
        ▼
16. Add optional Rigel MMIO routing
        │
        ▼
17. Validate chipset accesses
        │
        ▼
18. Optimize only after correctness

⸻

37. Refactor Before Semantic Expansion

The first milestone should not immediately add new hardware behavior.

First establish:

current behavior
      │
      ▼
new target boundary
      │
      ▼
minimal Bellatrix Bus
      │
      ▼
same behavior

Only then:

minimal Bellatrix Bus
      │
      ├── add Expansion
      └── add Rigel routing

This makes regressions much easier to identify.

⸻

38. Expansion as the First Architectural Validation

Expansion is an especially useful first validation because its expected M68K interface is simple and historically defined.

The test path becomes:

AROS
  │
  ▼
standard Expansion access
  │
  ▼
$E80000
  │
  ▼
intentional MMU trap
  │
  ▼
Emu68 fault reconstruction
  │
  ▼
Bellatrix Bus
  │
  ▼
Autoconfig semantics
  │
  ▼
AROS observes expansion hardware

If this works without an Emu68-specific AROS Expansion implementation, the target/bus boundary has demonstrated its intended purpose.

⸻

39. Rigel as the Second Architectural Validation

Rigel then validates a more complex hardware domain.

M68K software
    │
    ▼
classic chipset address
    │
    ▼
intentional MMU trap
    │
    ▼
Emu68
    │
    ▼
Bellatrix Bus
    │
    ▼
Rigel adapter
    │
    ▼
librigel

The Bellatrix Bus should need only enough knowledge to identify the Rigel compatibility domain.

Detailed chipset semantics remain inside Rigel.

⸻

40. Conformance Requirements

The architecture is successful when the following are true.

Emu68 independence

Generic Emu68 exception code does not contain Bellatrix Expansion or Rigel register semantics.

Target isolation

PiStorm, bare-metal non-PiStorm, and Bellatrix remain independently selectable machine environments.

MMU synchronization

Every Bellatrix Bus range is intentionally routed there by the Bellatrix MMU policy.

Bus independence

The Bellatrix Bus exists and operates without Rigel.

Expansion independence

Expansion operates without Rigel.

Rigel independence

Rigel does not include or depend on Emu68 exception internals.

Transaction preservation

The M68K-visible address, width, direction, value, ordering, and occurrence survive the Emu68-to-Bellatrix boundary correctly.

Standard AROS behavior

AROS can access Bellatrix Expansion through the normal classic hardware interface.

No generic provider requirement

The implementation does not require a dynamic provider framework merely to express known Bellatrix machine domains.

Legacy reuse

The legacy implementation is used to identify proven mechanisms where appropriate rather than unnecessarily redesigning the fault transport from first principles.

⸻

41. Review Checklist

Every patch affecting this path should answer:

1. Is this code generic Emu68 mechanism or Bellatrix machine semantics?
2. Does it belong in vectors.c, the target hook, or the Bellatrix Bus?
3. Is MMU policy synchronized with bus decode?
4. Can this address accidentally bypass the bus through direct mapping?
5. Is an intentionally trapped hardware address being confused with an invalid address?
6. Does Emu68 reconstruct the transaction without interpreting Bellatrix hardware?
7. Does Bellatrix interpret the machine transaction without duplicating ARM fault decoding?
8. Is Expansion independent from Rigel?
9. Is Rigel optional?
10. Is Rigel register knowledge leaking into Bellatrix or Emu68?
11. Is Expansion knowledge leaking into generic Emu68?
12. Is the bus becoming a generic provider framework unnecessarily?
13. Is execution topology leaking into the bus contract?
14. Is timing incorrectly becoming a responsibility of the bus?
15. Are interrupts incorrectly becoming a responsibility of the bus?
16. Is DMA incorrectly being modeled as reverse MMIO?
17. Does PiStorm behavior remain unchanged?
18. Does bare-metal non-PiStorm behavior remain unchanged?
19. Has the equivalent legacy path been inspected before introducing a new mechanism?
20. Is the new implementation simpler than the legacy mechanism while preserving its useful architectural property?

⸻

42. Target Architecture

The resulting architecture should be:

                         M68K software
                              │
                              ▼
                         Emu68 execution
                              │
                              ▼
                             MMU
                              │
                 ┌────────────┴────────────┐
                 │                         │
           direct mapping          intentional trap
                 │                         │
                 ▼                         ▼
            guest memory              Data Abort
                                           │
                                           ▼
                                  generic vectors.c
                                           │
                                           ▼
                                  fault reconstruction
                                           │
                                           ▼
                                  Bellatrix target hook
                                           │
                                           ▼
                                     Bellatrix Bus
                                           │
                       ┌───────────────────┼───────────────────┐
                       │                   │                   │
                  Expansion           optional Rigel       Bellatrix
                       │                   │                native
                       ▼                   ▼
                  Autoconfig         Rigel adapter
                                           │
                                           ▼
                                        librigel

The corresponding ownership model is:

Emu68
    owns execution and fault mechanism
Bellatrix target
    owns Bellatrix machine definition
Bellatrix MMU policy
    determines which accesses trap
Bellatrix Bus
    routes trapped machine transactions
Expansion
    owns Autoconfig semantics
Rigel
    owns classic chipset semantics

⸻

43. Final Direction

The recommended direction is not to invent a new Bellatrix/Emu68 hardware transport from scratch.

The Bellatrix legacy branch already demonstrates the essential mechanism:

Emu68 fault
     │
     ▼
Bellatrix hook
     │
     ▼
Bellatrix bus
     │
     ▼
machine semantics

That mechanism should be studied and reduced to its architectural minimum.

The new implementation should preserve:

one explicit Bellatrix target hook in the Emu68 fault path

and:

one minimal Bellatrix Bus boundary for M68K-visible machine transactions.

It should not automatically preserve the historical execution topology or synchronization machinery surrounding that boundary.

The resulting direction is:

legacy implementation
       │
       │ extract proven hook
       ▼
Emu68 fault reconstruction
       │
       ▼
Bellatrix target hook
       │
       ▼
minimal synchronous Bellatrix Bus
       │
       ├── Expansion
       ├── optional Rigel
       └── other Bellatrix machine semantics

The MMU and bus must remain synchronized:

Bellatrix machine definition
           │
     ┌─────┴─────┐
     │           │
 direct       hardware
 memory      transaction
     │           │
     ▼           ▼
    MMU     intentional trap
                 │
                 ▼
           Bellatrix Bus

Expansion should be the first architectural validation of this path.

Rigel should be the second.

Only after those paths are correct should more advanced concerns such as multicore transport, fine-grained bus contention, or other optimizations be introduced.

The central rule is:

Emu68 reconstructs the access. Bellatrix interprets the machine.

And the legacy branch should be used as the implementation reference for recovering the proven hook that connects those two responsibilities.
