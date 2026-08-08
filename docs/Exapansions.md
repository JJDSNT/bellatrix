Bellatrix Expansion and Autoconfig Architecture

Classic Expansion Semantics for AROS m68k-emu68

Status: Proposed Architectural Baseline
Scope: Bellatrix / Emu68 / AROS Expansion Integration
Target: Classic Amiga Expansion and Autoconfig Compatibility

⸻

1. Purpose

This document defines the architectural model for classic Amiga Expansion and Autoconfig support in the Bellatrix m68k-emu68 environment.

The primary objective is to prevent the m68k-emu68 AROS target from developing or maintaining a third, platform-specific interpretation of Amiga Expansion semantics.

The architectural rule is:

m68k-emu68 must either reuse the standard AROS Amiga Expansion implementation or use the generic non-Amiga Expansion stubs.

It must not maintain an independent Expansion/Autoconfig implementation.

Conceptually:

                       AROS m68k-emu68
                              │
                    ┌─────────┴─────────┐
                    │                   │
             no classic             classic
          Expansion domain       Expansion domain
                    │                   │
                    ▼                   ▼
             generic stubs       m68k-amiga Expansion
                                        │
                                        ▼
                              classic Autoconfig
                                   semantics

The choice between these two models must be determined by the hardware environment Bellatrix exposes to the M68K guest.

⸻

2. Architectural Principle

The AROS target must not invent platform-specific Expansion semantics merely because the M68K CPU executes through Emu68.

The execution engine and the guest-visible machine architecture are separate concerns.

Conceptually:

AROS
 │
 ▼
M68K machine semantics
 │
 ▼
Expansion / Autoconfig
 │
 ▼
M68K-visible address space
 │
 ▼
Emu68
 │
 ▼
Bellatrix

Whether the CPU instruction performing an Expansion access is:

* interpreted;
* dynamically translated;
* fault-dispatched;
* MMU-assisted;
* executed against physical hardware;

must not change the Expansion protocol visible to AROS.

Therefore:

Emu68 is an execution mechanism. It must not, by itself, require a new AROS Expansion model.

⸻

3. The Two Valid Models

There are only two architecturally valid models for m68k-emu68.

3.1 Model A — No Classic Expansion Domain

If Bellatrix intentionally exposes no classic Amiga Expansion/Autoconfig environment, the AROS target should behave like other targets without such hardware.

Conceptually:

AROS m68k-emu68
       │
       ▼
generic Expansion stubs
       │
       ▼
no classic Autoconfig bus

In this model, the absence of Expansion is intentional.

The stubs express:

This machine does not provide the classic Amiga Expansion environment.

They must not be used merely as a workaround for an unimplemented $E80000 path.

⸻

3.2 Model B — Classic Amiga Expansion Domain

If Bellatrix exposes the classic Expansion environment, m68k-emu68 should reuse the standard AROS Amiga Expansion implementation.

Conceptually:

AROS m68k-emu68
       │
       ▼
m68k-amiga Expansion implementation
       │
       ▼
classic Autoconfig accesses
       │
       ▼
M68K-visible Expansion address space
       │
       ▼
Emu68 / Bellatrix
       │
       ▼
Expansion backend

From the perspective of AROS, this should behave as an Amiga Expansion environment.

The fact that no physical Zorro bus exists is irrelevant at this architectural level.

⸻

4. The Invalid Third Model

The following architecture should explicitly be avoided:

m68k-amiga
    │
    └── classic Expansion implementation
generic targets
    │
    └── Expansion stubs
m68k-emu68
    │
    └── independent Emu68-specific
        Expansion implementation

This creates three semantic implementations for what should be a binary architectural decision:

classic Amiga Expansion exists
or
classic Amiga Expansion does not exist

An independent m68k-emu68 implementation risks introducing:

* duplicated Autoconfig logic;
* subtly different board enumeration behavior;
* different address assumptions;
* target-specific bugs;
* divergence from classic AROS behavior;
* unnecessary maintenance;
* Bellatrix-specific semantics leaking into AROS.

The target name must not determine the Expansion protocol.

The exposed machine architecture must determine it.

⸻

5. Preferred Bellatrix Model

The preferred Bellatrix architecture is to expose a sufficiently compatible classic Expansion domain and therefore allow AROS to reuse the standard Amiga implementation.

Conceptually:

                     AROS
                      │
                      ▼
          standard Amiga Expansion code
                      │
                      ▼
             classic Autoconfig
                      │
                      ▼
                $00E80000
                      │
                      ▼
              M68K-visible access
                      │
                      ▼
                    Emu68
                      │
                      ▼
             Bellatrix handling
                      │
                      ▼
             Expansion subsystem

The important property is not how $E80000 is implemented internally.

The important property is:

An M68K access to the classic Expansion address space receives the behavior expected by the standard Amiga Expansion implementation.

⸻

6. The $E80000 Boundary

Classic Autoconfig uses the Expansion configuration space beginning at:

$00E80000

For Bellatrix, this address should be treated as part of the guest-visible machine architecture when classic Expansion support is enabled.

The logical relationship is:

AROS Expansion
      │
      ▼
M68K read/write
      │
      ▼
$00E80000 Expansion space
      │
      ▼
Emu68 address handling
      │
      ▼
Bellatrix Expansion provider
      │
      ▼
Autoconfig semantics

AROS should not need to know how Bellatrix implements the lower layers.

⸻

7. $E80000 Must Not Be Ordinary RAM

The Expansion configuration area is not ordinary guest memory.

An access such as:

read $00E80000

must not simply resolve to:

guest RAM[$00E80000]

Likewise, writes into the Autoconfig space are hardware transactions rather than ordinary memory writes.

Conceptually:

M68K access
    │
    ▼
$E80000 range
    │
    ▼
Expansion provider
    │
    ▼
Autoconfig state machine

The address represents an architectural hardware interface.

⸻

8. $E80000 Must Not Be Treated as Unmapped When Expansion Exists

If Bellatrix declares that a classic Expansion domain exists, $E80000 cannot simultaneously behave as an ordinary unmapped region.

That would produce the contradictory architecture:

Classic Expansion enabled
          │
          ▼
AROS probes $E80000
          │
          ▼
unmapped

The same applies to treating the region as generic open bus without considering the state of the Expansion chain.

When Expansion exists, accesses must be resolved according to the defined Autoconfig state.

The response may depend on whether a board is currently participating in configuration.

Conceptually:

$E80000 access
      │
      ▼
Expansion subsystem
      │
      ▼
current Autoconfig state
      │
      ├── board available
      │       │
      │       ▼
      │   board configuration data
      │
      └── no board available
              │
              ▼
        defined classic
        no-board behavior

The exact no-board electrical/bus behavior must follow the compatibility semantics selected for Bellatrix rather than being invented as an Emu68-specific API behavior.

⸻

9. No-Board State Is Different from No Expansion Domain

These two conditions must remain distinct.

No Expansion Domain

machine architecture
       │
       ▼
does not expose classic Expansion
       │
       ▼
AROS uses generic stubs

Expansion Domain With No Board Present

machine architecture
       │
       ▼
classic Expansion exists
       │
       ▼
AROS uses Amiga Expansion implementation
       │
       ▼
Autoconfig probes bus
       │
       ▼
no board currently responds

The second condition is normal hardware state inside an existing Expansion architecture.

It must not be confused with the first.

⸻

10. Provider Selection Belongs Below AROS Expansion

AROS should issue the same logical accesses it would issue on the classic target.

The lower environment determines that the address belongs to the Expansion subsystem.

Conceptually:

M68K address
     │
     ▼
Emu68 / Bellatrix
address handling
     │
     ├── ordinary memory
     │
     ├── native platform device
     │
     ├── Expansion
     │       │
     │       ▼
     │   Autoconfig
     │
     └── genuinely unmapped

The standard AROS Expansion code should not perform Bellatrix-specific provider selection.

⸻

11. Emu68 MMU and Fault Handling

The mechanism used by Emu68 to intercept or dispatch accesses is an implementation detail below the AROS Expansion contract.

A possible implementation is:

M68K instruction
      │
      ▼
Emu68 translated execution
      │
      ▼
access $E80000
      │
      ▼
MMU / fault / address dispatch
      │
      ▼
Bellatrix Expansion handler
      │
      ▼
Autoconfig response
      │
      ▼
M68K-visible result

However, this document does not require that the implementation use an MMU fault.

The implementation could instead use another Emu68/Bellatrix address-dispatch mechanism if that mechanism preserves the required semantics.

Therefore:

MMU fault handling is a possible implementation mechanism, not part of the Expansion architecture exposed to AROS.

The architectural requirement is the M68K-visible behavior of the Expansion address space.

⸻

12. Do Not Encode Emu68 Into the Expansion Protocol

The Expansion implementation should not contain logic such as:

if Emu68
    perform special Autoconfig algorithm
else
    perform Amiga Autoconfig algorithm

Instead:

standard Autoconfig algorithm
           │
           ▼
M68K-visible hardware accesses
           │
           ▼
platform supplies semantics

This keeps the dependency in the correct direction.

AROS understands Amiga Expansion.

Bellatrix understands how that Expansion environment is realized on its host.

⸻

13. Responsibility Boundary

The intended ownership is:

AROS Expansion implementation
          │
          ├── ConfigDev management
          ├── ConfigChain logic
          ├── board enumeration
          ├── Autoconfig protocol
          └── OS-visible Expansion state
                  │
                  ▼
          M68K hardware accesses
                  │
                  ▼
        Emu68 / Bellatrix boundary
                  │
                  ▼
       Expansion hardware provider
                  │
          ├── configuration space
          ├── board response
          ├── configuration state
          └── address assignment effects

Bellatrix must not reproduce the OS-level functionality of expansion.library.

AROS must not reproduce Bellatrix’s address-dispatch mechanism.

⸻

14. Expansion Is Not Rigel

Classic Expansion support is outside the Rigel chipset boundary.

Rigel owns classic chipset hardware such as:

Agnus
Denise
Paula
CIA
Copper
Blitter
classic chipset DMA
classic chipset interrupts

Expansion represents a separate machine domain.

Conceptually:

                    M68K
                     │
          ┌──────────┼──────────┐
          │          │          │
          ▼          ▼          ▼
        RAM        Rigel     Expansion
                     │          │
                     ▼          ▼
                 chipset     Autoconfig /
                 hardware    expansion boards

Therefore:

Expansion must not be routed through Rigel merely because both represent classic Amiga hardware.

Bellatrix owns the composition of these independent machine domains.

⸻

15. Expansion Provider Architecture

Bellatrix should conceptually expose Expansion through its own provider.

                 Bellatrix address space
                         │
          ┌──────────────┼──────────────┐
          │              │              │
         RAM           Rigel        Expansion
          │              │              │
          ▼              ▼              ▼
      guest RAM       chipset       Autoconfig
                      regions        provider

This allows Expansion support to evolve independently from the chipset implementation.

It also preserves the possibility that:

CONFIG_RIGEL=n

while Expansion remains available.

Likewise, Bellatrix may potentially support configurations where Rigel exists but no Expansion provider is enabled.

⸻

16. Expansion Boards

Individual Expansion boards should sit behind the Expansion subsystem rather than being encoded into AROS.

Conceptually:

                    Expansion
                        │
                        ▼
                Autoconfig chain
                        │
          ┌─────────────┼─────────────┐
          │             │             │
       Board A       Board B       Board C
          │             │             │
          ▼             ▼             ▼
       device         device         device
       backend        backend        backend

The guest discovers these boards through standard Expansion semantics.

AROS should not require Bellatrix-specific knowledge to identify them.

⸻

17. Board Discovery

The preferred relationship is:

AROS
 │
 ▼
standard expansion.library
 │
 ▼
standard Autoconfig enumeration
 │
 ▼
$E80000
 │
 ▼
currently visible board
 │
 ▼
configuration information
 │
 ▼
AROS configures board
 │
 ▼
next board becomes visible
 │
 ▼
repeat

This allows the guest to discover Bellatrix-provided expansion devices in the same architectural manner as classic hardware.

⸻

18. Board Configuration State

The Expansion provider should own the hardware-side state of the Autoconfig chain.

Conceptually:

Expansion provider
        │
        ├── board list
        │
        ├── current board
        │
        ├── configuration-space state
        │
        ├── configured address
        │
        └── shut-up/configured state

AROS owns the OS-side interpretation of that state.

The distinction is:

hardware-side Autoconfig state
            │
            ▼
         Bellatrix
OS-side Expansion state
            │
            ▼
     expansion.library

⸻

19. Assigned Board Address Space

Once a board is configured, accesses to its assigned address range should no longer be interpreted as accesses to the $E80000 configuration window.

Conceptually:

before configuration
$E80000
   │
   ▼
Board A Autoconfig
after configuration
Board A assigned range
   │
   ▼
Board A runtime device
$E80000
   │
   ▼
Board B Autoconfig

The Bellatrix address-space architecture must therefore support the transition between:

configuration visibility

and:

runtime device visibility

without requiring special knowledge in AROS.

⸻

20. Runtime Board Providers

A configured Expansion board may expose its own runtime MMIO or memory region.

Conceptually:

M68K address
     │
     ▼
Bellatrix address dispatcher
     │
     ├── RAM
     │
     ├── Rigel
     │
     ├── Expansion configuration
     │
     ├── configured Board A
     │
     ├── configured Board B
     │
     └── unmapped

The Expansion subsystem therefore participates in machine address-space composition but does not need to become a global address dispatcher itself.

⸻

21. Zorro Semantics

The Expansion architecture should preserve the distinction between:

Autoconfig protocol
and
runtime board behavior

A board may participate in Autoconfig and subsequently expose:

* MMIO;
* RAM;
* DMA;
* interrupts;
* another device-specific interface.

The Expansion subsystem provides discovery and configuration semantics.

The board implementation provides runtime device semantics.

Conceptually:

               Expansion subsystem
                       │
                       ▼
                   Autoconfig
                       │
                       ▼
                configured board
                       │
             ┌─────────┼─────────┐
             │         │         │
            MMIO      DMA       IRQ

⸻

22. DMA

Expansion-board DMA must remain separate from Rigel chipset DMA.

Conceptually:

Rigel DMA
   │
   ▼
classic chipset address semantics
Expansion-board DMA
   │
   ▼
board-specific bus semantics
```
Both may eventually reach Bellatrix guest memory, but their architectural origin is different.
Therefore:
> A common guest-memory backend does not imply a common hardware subsystem.
---
# 23. Interrupts
Expansion-board interrupts similarly remain outside Rigel.
Conceptually:
~~~text
Rigel
  │
  ▼
rigel_ipl
  │
  │
  ├──────────────┐
                 ▼
            IPL arbitration
                 ▲
                 │
Expansion IRQ ───┘

The exact Bellatrix interrupt architecture is outside the scope of this document.

The important rule is that Expansion interrupts must not be routed through Rigel merely to reach the M68K CPU.

⸻

24. Reset

Expansion reset semantics should belong to the Expansion subsystem and individual boards.

Conceptually:

Bellatrix machine reset
          │
          ├── Rigel reset
          │
          ├── Expansion reset
          │       │
          │       └── reset Autoconfig chain
          │
          └── other machine domains

Rigel reset must not implicitly reset Expansion.

Expansion reset must not depend on Rigel internal state.

Bellatrix coordinates machine-level reset across the independent domains.

⸻

25. AROS Source Reuse

The desired AROS architecture is:

                 AROS source tree
                       │
            ┌──────────┴──────────┐
            │                     │
     generic Expansion       Amiga Expansion
          stubs             implementation
            │                     │
            │                     │
            └──────────┬──────────┘
                       │
                 target selects
                       │
                       ▼
                  m68k-emu68

The m68k-emu68 target should select one of the existing semantic models.

It should not fork either into a third implementation.

⸻

26. Code Sharing Versus Source Duplication

Reusing the Amiga implementation does not necessarily mean physically copying its source files into the m68k-emu68 directory.

The preferred solution should minimize duplication.

Possible mechanisms include:

shared source
common implementation directory
build-system source selection
thin target wrapper around shared code

The exact AROS build-system mechanism should be selected according to the existing source-tree conventions.

The architectural requirement is:

There must be one implementation of classic AROS Autoconfig semantics, not separate Amiga and Emu68 implementations that happen to behave similarly.

⸻

27. Existing m68k-emu68 Expansion Code

Any existing m68k-emu68-specific Expansion implementation should therefore be treated as transitional code.

It should be audited against:

m68k-amiga Expansion implementation
```
and:
~~~text
generic target Expansion stubs

Every difference should be classified.

Conceptually:

m68k-emu68 difference
        │
        ├── actual required platform adaptation
        │       │
        │       ▼
        │   move below the Expansion
        │   semantic boundary where possible
        │
        ├── generic behavior
        │       │
        │       ▼
        │   use generic stub
        │
        └── classic Amiga behavior
                │
                ▼
            reuse Amiga code

The default assumption should not be that existing target-specific code must be preserved.

⸻

28. Platform Adaptation Belongs Below the Protocol

If an existing m68k-emu68 implementation contains code that is genuinely required because of Emu68, the first question should be:

Can this difference be moved below the AROS Expansion protocol?

For example:

AROS Autoconfig code
        │
        ▼
M68K access
        │
        ▼
Emu68-specific handling

is preferable to:

AROS Emu68-specific Autoconfig algorithm
        │
        ▼
special platform behavior

This keeps classic hardware semantics reusable.

⸻

29. MMU Handling Must Not Define Guest Semantics

If the Emu68 MMU is used to implement the Expansion range, its behavior must be subordinate to the Expansion contract.

The wrong dependency is:

Emu68 MMU happens to return X
        │
        ▼
therefore Expansion behaves as X

The correct dependency is:

Expansion requires behavior X
        │
        ▼
Bellatrix chooses implementation
        │
        ▼
Emu68 MMU/fault machinery
implements X

Host implementation convenience must not accidentally define the guest-visible architecture.

⸻

30. Unmapped and Open-Bus Behavior

The terms:

unmapped
open bus
no board present

must not be treated as synonyms.

They represent potentially different architectural conditions.

Conceptually:

unmapped
    │
    └── no provider owns this address
Expansion no-board state
    │
    └── Expansion provider owns the address,
        but no board currently responds
open-bus behavior
    │
    └── possible electrical/bus-level result
        of a defined hardware condition

If Bellatrix exposes classic Expansion, $E80000 belongs to the Expansion architecture even when no board is currently available.

The exact guest-visible result of a no-board probe must be defined according to the classic compatibility behavior chosen for Bellatrix.

It must not accidentally inherit the generic Emu68 unmapped-memory behavior.

⸻

31. Architectural Test

A useful conceptual test is:

Could the standard AROS Amiga Expansion implementation run unchanged against the Bellatrix Expansion environment?

If the answer is yes, the boundary is likely correct.

If the answer is no, determine why.

A legitimate reason might expose a missing hardware semantic in Bellatrix.

It should not automatically justify creating a separate AROS implementation.

Conceptually:

standard AROS Amiga Expansion
             │
             ▼
         Bellatrix
             │
             ▼
          works?
          /    \
        yes     no
        │        │
        ▼        ▼
      good    identify missing
      boundary hardware semantic
                    │
                    ▼
             fix lower boundary
```
Only if the machine intentionally does not expose classic Expansion should the answer instead be:
~~~text
use generic stubs

⸻

32. Implementation Investigation

Before changing the AROS source tree, the existing Bellatrix and Emu68 memory paths should be inspected to determine how $E80000 currently behaves.

The investigation should answer:

1. Is $00E80000 currently mapped?
2. If mapped, which component owns it?
3. If unmapped, what does Emu68 currently do when translated M68K code accesses it?
4. Does the access reach an Emu68 fault handler?
5. Can that handler delegate the access to a Bellatrix provider?
6. Are reads and writes distinguishable?
7. Are byte and word accesses preserved correctly?
8. Can the provider return an M68K-visible read value?
9. Can the provider maintain Autoconfig state between accesses?
10. Can configured boards install or expose their runtime address ranges?
11. What currently happens when no Expansion board is present?
12. Does any existing Emu68 board mechanism already provide useful infrastructure for this path?

These questions determine the implementation mechanism.

They do not determine the guest architecture.

⸻

33. Recommended Implementation Direction

Assuming Bellatrix intends to support classic Expansion, the recommended direction is:

1. Audit existing AROS m68k-emu68 Expansion code
        │
2. Compare with m68k-amiga implementation
        │
3. Remove independent Autoconfig semantics
        │
4. Reuse standard Amiga Expansion implementation
        │
5. Inspect Emu68 handling of $E80000
        │
6. Establish Bellatrix Expansion provider
        │
7. Route $E80000 accesses to that provider
        │
8. Implement classic no-board behavior
        │
9. Implement board Autoconfig state
        │
10. Implement configured board address exposure
        │
11. Boot AROS using shared Amiga Expansion code
        │
12. Validate board enumeration

The critical transition is:

fix AROS for Emu68

becoming:

make Bellatrix expose the hardware
that normal AROS expects

⸻

34. Initial Scope

The first implementation does not need to model every possible Zorro behavior.

The minimum useful scope is:

classic Expansion configuration window
Autoconfig enumeration
no-board behavior
one or more Bellatrix-provided boards
address assignment
configured runtime MMIO
reset of Autoconfig chain

Advanced features may follow later:

board DMA
complex interrupt routing
Zorro bus timing
multiple bus generations
advanced contention
hot-plug-like development facilities

Correct architectural ownership should precede completeness.

⸻

35. What Must Not Happen

The implementation should explicitly avoid:

AROS m68k-emu68
        │
        └── Bellatrix-specific Expansion protocol

It should also avoid:

Rigel
  │
  └── Expansion implementation

and:

$E80000
   │
   └── ordinary RAM

and, when Expansion is enabled:

$E80000
   │
   └── generic unmapped handler

Likewise, Bellatrix should not implement OS-side expansion.library behavior.

The desired separation is:

AROS
 │
 └── OS Expansion semantics
Bellatrix
 │
 └── hardware Expansion semantics
Emu68
 │
 └── execution/address-access mechanism

⸻

36. Conformance Requirements

The architecture should be considered successful when the following properties hold.

No third AROS implementation

m68k-emu68 does not maintain independent Autoconfig semantics.

Standard implementation reuse

When classic Expansion is enabled, the AROS target uses the same classic Expansion logic as the Amiga target.

Stub correctness

When classic Expansion is intentionally absent, the target uses the generic no-Expansion behavior rather than an Emu68-specific substitute.

Guest transparency

AROS does not need to know whether $E80000 is implemented through physical hardware, MMU faults, translated-access dispatch, or another host mechanism.

Address ownership

When classic Expansion is enabled, $E80000 is owned by the Expansion subsystem.

No-board correctness

An empty Autoconfig chain is represented as a valid Expansion hardware state rather than automatically as absence of the Expansion architecture.

Provider isolation

Expansion is independent from Rigel.

Protocol ownership

AROS owns OS-side Expansion enumeration logic.

Bellatrix owns guest-visible Expansion hardware behavior.

Execution-engine independence

Emu68 does not define a separate Autoconfig protocol.

Board isolation

Individual expansion-board runtime behavior remains separate from the core Autoconfig protocol.

Reset isolation

Expansion reset semantics do not depend on Rigel reset internals.

⸻

37. Review Checklist

Every Expansion-related change should answer:

1. Is this classic Amiga Expansion behavior or genuinely Emu68-specific behavior?
2. If it is classic behavior, why is it not using the existing Amiga implementation?
3. If there is no classic Expansion domain, why is the generic stub insufficient?
4. Is a third Autoconfig algorithm being introduced?
5. Is Bellatrix-specific knowledge leaking into AROS?
6. Is AROS-specific knowledge leaking into the Bellatrix hardware provider?
7. Is $E80000 being treated as ordinary memory?
8. Is $E80000 incorrectly reaching a generic unmapped handler when Expansion is enabled?
9. Is no-board state being confused with no Expansion domain?
10. Is generic open-bus behavior being confused with defined Expansion no-board behavior?
11. Is Emu68 MMU behavior accidentally defining the guest architecture?
12. Could the implementation mechanism below $E80000 be replaced without changing AROS?
13. Is Expansion being incorrectly placed inside Rigel?
14. Are Expansion-board DMA and Rigel DMA being confused?
15. Are Expansion interrupts being routed through Rigel without architectural reason?
16. Does the standard AROS Amiga Expansion implementation work against this boundary?
17. If not, what exact guest-visible hardware semantic is missing?
18. Can that semantic be implemented below the AROS Expansion layer instead of forking the OS code?

If these questions cannot be answered cleanly, the boundary should be reconsidered.

⸻

38. Target Architecture

The target architecture is:

                         AROS
                           │
                           ▼
                standard expansion.library
                           │
                           ▼
               classic Amiga Autoconfig
                           │
                           ▼
                    M68K accesses
                           │
                           ▼
                         Emu68
                           │
                           ▼
              Bellatrix address handling
                           │
            ┌──────────────┼──────────────┐
            │              │              │
           RAM           Rigel        Expansion
                                           │
                                           ▼
                                      $E80000
                                           │
                                           ▼
                                  Autoconfig chain
                                           │
                          ┌────────────────┼────────────────┐
                          │                │                │
                       Board A          Board B          Board C
                          │                │                │
                          ▼                ▼                ▼
                       runtime          runtime          runtime
                       provider         provider         provider

The essential property is:

AROS sees an Amiga Expansion environment.
Emu68 executes the accesses.
Bellatrix provides the hardware semantics.

No additional AROS Expansion architecture exists specifically for Emu68.

⸻

39. Decision Rule

The target selection rule should be explicit:

Does Bellatrix expose classic
Amiga Expansion semantics?
             │
           /   \
         yes    no
         │       │
         ▼       ▼
   reuse AROS   use generic
   m68k-amiga   Expansion
   Expansion    stubs
```
There is intentionally no third branch.
Not:
~~~text
             ┌── Amiga implementation
             │
m68k-emu68 ──┼── generic stub
             │
             └── Emu68 implementation

But:

             ┌── Amiga implementation
             │
m68k-emu68 ──┤
             │
             └── generic stub

The hardware architecture determines which branch is correct.

⸻

40. Current Architectural Decision

For Bellatrix, the preferred direction is:

Expose classic Amiga Expansion/Autoconfig semantics and reuse the standard AROS Amiga Expansion implementation.

Therefore the expected target is:

AROS m68k-emu68
       │
       ▼
shared m68k-amiga Expansion logic
       │
       ▼
$00E80000
       │
       ▼
Emu68 access mechanism
       │
       ▼
Bellatrix Expansion provider
       │
       ▼
classic Autoconfig semantics

The remaining implementation question is not whether AROS should have an Emu68-specific Expansion protocol.

It should not.

The remaining question is:

What is the cleanest Emu68/Bellatrix mechanism for providing the required $E80000 semantics to the unchanged classic AROS Expansion implementation?

That question should be answered by inspecting the existing Emu68 MMU, fault-handling, address-dispatch, and board infrastructure.

⸻

41. Final Recommendation

Bellatrix should treat classic Expansion as a machine architecture boundary, not as an AROS port workaround.

The architectural layers should remain:

AROS
 │
 │ standard Amiga Expansion semantics
 ▼
M68K-visible Expansion interface
 │
 │ $E80000 / Autoconfig
 ▼
Emu68
 │
 │ execution and access mechanism
 ▼
Bellatrix
 │
 │ hardware provider
 ▼
Expansion boards

The governing rules are:

There must not be a third m68k-emu68 Expansion implementation.

If Bellatrix exposes no classic Expansion domain, use the generic AROS stubs.

If Bellatrix exposes classic Expansion, reuse the standard AROS Amiga Expansion implementation.

The preferred Bellatrix architecture is the latter.

$E80000 is a hardware interface, not ordinary RAM.

When Expansion exists, $E80000 must not accidentally inherit generic unmapped-memory behavior.

An empty Expansion chain and the absence of an Expansion architecture are different states.

The exact no-board response must be defined according to the selected classic compatibility semantics.

Emu68 MMU or fault handling may implement the boundary, but must not define its semantics.

Emu68 is the execution mechanism, not a third Expansion architecture.

Expansion is independent from Rigel.

Bellatrix provides hardware semantics; AROS provides OS Expansion semantics.

The desired outcome is therefore not:

make expansion.library understand Bellatrix

but:

make Bellatrix expose an Expansion environment
that the standard expansion.library already understands
