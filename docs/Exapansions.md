# Bellatrix / AROS Classic Expansion Integration

## Shared Autoconfig Implementation with Runtime Hardware Detection

**Status:** Proposed Architectural Baseline  
**Scope:** AROS m68k / m68k-emu68 / Bellatrix  
**Target:** Optional Classic Amiga Expansion Compatibility

---

# 1. Purpose

This document defines how classic Amiga Expansion and Autoconfig support should be integrated into the AROS `m68k-emu68` target used by Bellatrix.

The architecture is based on four rules:

1. There must be only one `expansion.library`.
2. There must be only one implementation of classic Amiga Autoconfig semantics.
3. The `m68k-emu68` target must not contain a copied or independent Expansion implementation.
4. The presence of the classic Expansion hardware domain must be detected at runtime by AROS.

The resulting architecture is:

~~~text
                     expansion.library
                            │
                            ▼
                 shared Expansion code
                            │
                            ▼
              classic hardware available?
                     /             \
                   yes              no
                    │                │
                    ▼                ▼
             shared classic      no classic
              Autoconfig         enumeration
                    │
                    ▼
             Expansion boards
~~~

The runtime decision determines whether the shared classic hardware implementation is used.

It does not select between different implementations of Expansion semantics.

---

# 2. Primary Architectural Rule

The fundamental rule is:

> There must never be a third Emu68-specific implementation of classic Amiga Expansion or Autoconfig semantics.

The architecture must not become:

~~~text
m68k-amiga
    │
    └── classic Autoconfig implementation

m68k-emu68
    │
    └── Emu68 Autoconfig implementation

generic targets
    │
    └── no-Expansion implementation
~~~

Instead:

~~~text
                     shared classic
                  Autoconfig implementation
                         ▲       ▲
                         │       │
                  m68k-amiga   m68k-emu68
                                  │
                             runtime probe
~~~

The M68K execution environment does not define a new Expansion protocol.

Emu68 is an execution mechanism.

Classic Amiga Expansion semantics remain classic Amiga Expansion semantics.

---

# 3. One `expansion.library`

The runtime detection mechanism must not result in multiple versions of `expansion.library`.

There should remain:

~~~text
one expansion.library
~~~

not:

~~~text
expansion.library.amiga

expansion.library.emu68

expansion.library.noexpansion
~~~

The common library continues to provide its normal OS-level functionality.

Whether classic hardware enumeration is performed is a runtime property of the machine on which AROS is currently executing.

Conceptually:

~~~text
                 expansion.library
                        │
              common library state
                        │
              common library API
                        │
                        ▼
                 initialization
                        │
                        ▼
              classic hardware probe
                   /           \
                 yes            no
                  │              │
                  ▼              ▼
            ConfigChain()      continue
                  │            without
                  ▼            classic
            shared classic     enumeration
             implementation
~~~

The absence of classic Expansion hardware does not require a different library binary.

---

# 4. One Classic Autoconfig Implementation

Classic Amiga Autoconfig semantics must exist in one source implementation.

This includes functionality conceptually equivalent to:

~~~text
ConfigChain

ConfigBoard

ReadExpansionRom

ReadExpansionByte

WriteExpansionByte

WriteExpansionWord

Autoconfig address assignment

board configuration state

Shutup semantics
~~~

The same implementation must be used whenever classic Amiga Expansion hardware is available.

The implementation must not be copied into:

~~~text
arch/m68k-emu68/expansion/
~~~

merely because the target executes through Emu68.

Instead, the classic implementation should be moved or organized so that it can be compiled and used by every M68K target that exposes compatible hardware.

Conceptually:

~~~text
                 shared M68K classic
                 Expansion implementation
                           │
                 ┌─────────┴─────────┐
                 │                   │
             m68k-amiga         m68k-emu68
                 │                   │
          always available      runtime probe
~~~

The exact source-tree location is an implementation decision.

Possible arrangements include:

~~~text
arch/m68k-all/expansion/
~~~

or another shared location consistent with AROS source-tree conventions.

The architectural requirement is source sharing, not a specific directory name.

---

# 5. Current m68k-emu68 Duplication Must Be Removed

The current `m68k-emu68` target contains copied classic Expansion implementation files.

That arrangement should be considered transitional.

The target architecture must not retain:

~~~text
arch/m68k-amiga/expansion/
        │
        └── classic implementation

arch/m68k-emu68/expansion/
        │
        └── copied classic implementation
~~~

The desired result is:

~~~text
shared classic Expansion implementation
                 │
          ┌──────┴──────┐
          │             │
     m68k-amiga     m68k-emu68
                         │
                    runtime probe
~~~

Any target-specific code remaining under `m68k-emu68` must represent genuine platform adaptation rather than duplicated classic Expansion semantics.

---

# 6. Runtime Detection

The `m68k-emu68` target must determine at runtime whether the currently running machine exposes the classic Amiga Expansion hardware domain.

The decision must not be compile-time policy such as:

~~~c
#define EMU68_AUTOCONFIG 0
~~~

or:

~~~c
#define EMU68_AUTOCONFIG 1
~~~

Instead:

~~~text
AROS boot
    │
    ▼
expansion.library initialization
    │
    ▼
runtime hardware probe
    │
    ├── classic Expansion present
    │       │
    │       ▼
    │   execute shared classic
    │   Autoconfig implementation
    │
    └── classic Expansion absent
            │
            ▼
        skip classic
        hardware enumeration
~~~

This allows the same AROS `m68k-emu68` binary to operate on machines with different hardware capabilities.

---

# 7. Runtime Probe Is Not a Third Implementation

The runtime probe is target-specific detection logic.

It is not an Expansion implementation.

The distinction is fundamental.

The `m68k-emu68` target may contain:

~~~text
detect whether classic Expansion hardware exists
~~~

It must not contain its own implementation of:

~~~text
ConfigChain

ConfigBoard

Autoconfig protocol

board enumeration

configuration-space decoding

address assignment

Shutup

Zorro semantics
~~~

Therefore:

~~~text
m68k-emu68-specific code
          │
          ▼
        probe
          │
       /     \
     yes      no
      │        │
      ▼        ▼
   shared     skip
   classic    enumeration
   code
~~~

There remain only two hardware states:

~~~text
classic Expansion available

classic Expansion unavailable
~~~

There are not three Expansion semantic implementations.

---

# 8. Probe Ownership

The runtime probe belongs to AROS.

Bellatrix must expose machine behavior.

AROS determines what hardware environment it is running on.

Conceptually:

~~~text
Bellatrix
    │
    │ exposes machine architecture
    ▼
M68K-visible environment
    │
    ▼
AROS runtime probe
    │
    ▼
classic Expansion available?
~~~

Bellatrix must not select which AROS Expansion implementation is compiled or loaded.

Likewise, Emu68 must not introduce an Emu68-specific Autoconfig protocol.

The dependency remains:

~~~text
AROS
 │
 │ detects hardware
 ▼
M68K-visible machine
 │
 ▼
Emu68
 │
 │ execution/access mechanism
 ▼
Bellatrix
```

---

# 9. Domain Detection and Board Detection Are Different

The runtime probe must conceptually distinguish two questions.

First:

~~~text
Does this machine expose
the classic Expansion domain?
~~~

Second:

~~~text
Does the classic Expansion
chain currently contain boards?
~~~

These questions must not be collapsed.

The valid states include:

~~~text
Expansion absent
~~~

and:

~~~text
Expansion present
    │
    └── zero boards
~~~

and:

~~~text
Expansion present
    │
    └── one or more boards
~~~

Therefore:

~~~text
runtime domain probe
        │
       / \
     no   yes
     │     │
     │     ▼
     │  classic ConfigChain
     │     │
     │    / \
     │   /   \
     │ board  no board
     │
     ▼
no classic enumeration
~~~

The classic `ConfigChain` implementation remains responsible for discovering boards.

The runtime platform probe determines only whether invoking the classic hardware path is valid.

---

# 10. Do Not Use Board Presence as Domain Presence

The runtime probe must not define:

~~~text
board responds at $E80000
        │
        ▼
Expansion exists
~~~

because this cannot distinguish:

~~~text
Expansion domain exists
but no board is present
~~~

from:

~~~text
Expansion domain does not exist
~~~

The probe must determine whether the hardware domain itself is available.

After that determination:

~~~text
domain present
      │
      ▼
shared classic ConfigChain
      │
      ▼
normal Autoconfig probing
      │
      ├── board present
      └── chain empty
~~~

The exact runtime mechanism used to determine domain presence must be defined separately according to the facilities available to the AROS `m68k-emu68` target.

---

# 11. No Compile-Time Bellatrix Expansion Selection

Bellatrix Expansion availability must not require separate AROS builds such as:

~~~text
AROS-emu68-with-expansion

AROS-emu68-without-expansion
~~~

The desired architecture is:

~~~text
same AROS m68k-emu68 image
          │
          ▼
       runtime
          │
         / \
        /   \
Bellatrix    Bellatrix
without      with
Expansion    Expansion
    │            │
    ▼            ▼
probe false   probe true
    │            │
    ▼            ▼
continue      shared classic
without       ConfigChain
enumeration
~~~

This allows Expansion to remain an optional machine capability.

---

# 12. Bellatrix Does Not Need Expansion

Classic Expansion is not required by the Bellatrix Core architecture.

A valid Bellatrix machine may expose:

~~~text
Bellatrix
    │
    ├── RAM
    ├── native devices
    ├── optional Rigel
    └── no classic Expansion
~~~

Another Bellatrix configuration may eventually expose:

~~~text
Bellatrix
    │
    ├── RAM
    ├── native devices
    ├── optional Rigel
    └── optional classic Expansion
~~~

The same AROS `m68k-emu68` binary should be capable of detecting the difference at runtime.

---

# 13. Expansion Remains Independent from Rigel

Classic Expansion is not part of Rigel.

The machine composition remains:

~~~text
                       M68K
                        │
          ┌─────────────┼─────────────┐
          │             │             │
         RAM          Rigel       Expansion
                        │             │
                        ▼             ▼
                    chipset       Autoconfig /
                    hardware      Zorro domain
~~~

Rigel owns classic chipset semantics.

Expansion, when present, represents a separate machine domain.

Therefore:

~~~text
CONFIG_RIGEL=n
~~~

must not imply:

~~~text
Expansion unavailable
~~~

and:

~~~text
Expansion unavailable
~~~

must not imply:

~~~text
Rigel unavailable
~~~

Bellatrix composes these domains independently.

---

# 14. Emu68 Remains an Execution Mechanism

Emu68 must not define a separate Expansion architecture.

Its responsibility is to execute M68K accesses and provide the lower mechanism through which Bellatrix can implement machine address-space semantics.

Conceptually:

~~~text
AROS classic Expansion code
           │
           ▼
       M68K access
           │
           ▼
         Emu68
           │
           ▼
Bellatrix address handling
           │
           ▼
Expansion hardware domain
~~~

Whether the access is implemented through:

~~~text
MMU faults

host bus hooks

direct dispatch

another Emu68 mechanism
~~~

does not alter the AROS Expansion protocol.

---

# 15. Target-Specific Adaptation

Removing duplicated classic Expansion code does not mean that all M68K targets must become identical.

Target-specific adaptation is allowed when it represents a genuine difference outside classic Autoconfig semantics.

The rule is:

> Share hardware semantics; isolate platform adaptation.

For example:

~~~text
shared classic code
       │
       ├── ConfigChain semantics
       ├── ConfigBoard semantics
       ├── Autoconfig protocol
       └── board enumeration
~~~

while:

~~~text
m68k-amiga adaptation
       │
       └── machine-specific environment

m68k-emu68 adaptation
       │
       ├── runtime Expansion-domain probe
       └── genuine platform-specific
           memory/platform handling
~~~

A platform difference must not justify copying the entire classic implementation.

---

# 16. Motherboard Memory Discovery Is Not Autoconfig Semantics

The existing `m68k-emu68` target may differ from classic Amiga targets in how ordinary machine RAM is discovered.

For example, Bellatrix/Emu68 may obtain usable RAM topology through platform description mechanisms rather than probing arbitrary classic motherboard memory regions.

Such differences should remain platform-specific.

They must not require duplication of the classic Autoconfig implementation.

Conceptually:

~~~text
                 expansion.library
                        │
             shared classic logic
                        │
            ┌───────────┴───────────┐
            │                       │
       m68k-amiga               m68k-emu68
            │                       │
     classic machine           platform-provided
     RAM discovery             RAM discovery
~~~

This distinction should be made explicitly when extracting the currently duplicated `m68k-emu68` Expansion code.

The objective is not blindly sharing every line.

The objective is sharing every piece of classic Expansion semantics.

---

# 17. Required Source Refactoring

The existing source arrangement should be refactored from:

~~~text
arch/m68k-amiga/expansion/
    configchain.c
    configboard.c
    readexpansionrom.c
    readexpansionbyte.c
    writeexpansionbyte.c
    writeexpansionword.c
    ...

arch/m68k-emu68/expansion/
    configchain.c
    configboard.c
    readexpansionrom.c
    readexpansionbyte.c
    writeexpansionbyte.c
    writeexpansionword.c
    ...
~~~

into a model conceptually equivalent to:

~~~text
shared classic M68K Expansion code/
    configchain.c
    configboard.c
    readexpansionrom.c
    readexpansionbyte.c
    writeexpansionbyte.c
    writeexpansionword.c
    ...

arch/m68k-amiga/
    classic Expansion binding
    platform-specific adaptation only

arch/m68k-emu68/
    runtime Expansion probe
    platform-specific adaptation only
~~~

The exact directory names are not normative.

What is normative is:

~~~text
one source implementation
of classic Autoconfig semantics
~~~

---

# 18. Desired Initialization Flow

The resulting initialization should conceptually be:

~~~text
expansion.library initialization
             │
             ▼
      initialize common state
             │
             ▼
     platform capability check
             │
            / \
           /   \
      classic   classic
      present   absent
         │         │
         ▼         │
 shared classic    │
   ConfigChain     │
         │         │
         └────┬────┘
              │
              ▼
      continue library init
~~~

For a classic Amiga target, the capability may effectively always be present.

For `m68k-emu68`, it is determined at runtime.

The classic implementation itself does not need to know why it was invoked.

---

# 19. Desired m68k-emu68 Responsibility

After refactoring, the `m68k-emu68` Expansion-specific code should be very small.

Conceptually it should answer:

~~~text
Is classic Expansion hardware
available on this running machine?
~~~

and provide only genuinely necessary platform adaptation.

It should not answer:

~~~text
How does Autoconfig work?

How is a ConfigDev constructed?

How does a board receive an address?

How is an Expansion ROM decoded?

How does Shutup work?

How does the configuration chain advance?
~~~

Those questions already have a classic implementation.

---

# 20. What Must Be Removed

The convergence should remove:

~~~text
copied m68k-amiga Expansion implementation
inside m68k-emu68
~~~

and:

~~~text
compile-time EMU68_AUTOCONFIG selection
~~~

and prevent the introduction of:

~~~text
Emu68-specific ConfigChain semantics
~~~

or:

~~~text
Bellatrix-specific expansion.library
~~~

The target-specific surface should become:

~~~text
runtime probe
+
minimal platform adaptation
~~~

---

# 21. What Must Be Preserved

The refactoring should preserve the existing classic AROS semantics for:

~~~text
ConfigChain

ConfigBoard

ConfigDev

Expansion ROM parsing

Autoconfig enumeration

address assignment

board configuration

Shutup

BoardList
~~~

where those semantics are already implemented by the standard classic Amiga path.

The objective is not to rewrite Autoconfig.

The objective is to stop copying it.

---

# 22. Runtime Probe Contract

The runtime probe should have a deliberately narrow semantic contract.

Conceptually:

~~~c
bool
platform_has_classic_expansion(void);
~~~

The exact API name and location are not normative.

Its semantic question is:

> Does the currently running machine expose a classic Amiga Expansion hardware domain that can safely be consumed by the shared classic Expansion implementation?

It must not mean:

> Is there currently a board responding in the Autoconfig chain?

Therefore:

~~~text
TRUE
 │
 └── classic hardware path may be used

FALSE
 │
 └── classic hardware path must not be accessed
~~~

A true result does not imply that any Expansion board exists.

---

# 23. Probe Implementation Is a Separate Investigation

This architecture deliberately does not prescribe the low-level mechanism used by the runtime probe.

Possible implementation mechanisms must be evaluated against the actual AROS m68k-emu68 and Bellatrix execution environment.

The investigation must determine how AROS can safely distinguish:

~~~text
classic Expansion domain present
but chain empty
~~~

from:

~~~text
classic Expansion domain absent
~~~

without turning absence into an uncontrolled CPU exception or relying on board presence as a proxy for domain presence.

The mechanism may involve existing machine/platform facilities, safe address probing, exception handling, or another runtime-visible machine property.

The architectural requirement is the runtime semantic result.

The implementation mechanism should be selected only after inspecting the relevant AROS/Emu68 path.

---

# 24. No New Bellatrix Protocol

The runtime probe must not become a new Bellatrix-specific Expansion protocol.

For example, the architecture should avoid turning normal Expansion operation into:

~~~text
AROS
 │
 ▼
Bellatrix Expansion API
 │
 ▼
Autoconfig
~~~

The normal path remains:

~~~text
AROS
 │
 ▼
shared classic Autoconfig code
 │
 ▼
M68K hardware accesses
 │
 ▼
machine
~~~

Only the determination of whether that hardware path is valid is platform-sensitive.

Once the shared classic implementation begins executing, it should observe ordinary classic Expansion semantics.

---

# 25. Bellatrix With No Expansion

For the initial Bellatrix architecture, it is valid for the runtime probe to determine:

~~~text
classic Expansion unavailable
~~~

The resulting behavior is simply:

~~~text
expansion.library
       │
       ▼
runtime probe
       │
       ▼
     false
       │
       ▼
skip classic ConfigChain
       │
       ▼
continue normal system initialization
~~~

Bellatrix does not need to implement `$E80000`, Zorro, Autoconfig, or Super Buster merely because `expansion.library` exists.

---

# 26. Future Bellatrix With Expansion

If Bellatrix later introduces an optional Expansion provider:

~~~text
Bellatrix
    │
    └── Expansion domain
            │
            ├── $E80000
            ├── Autoconfig chain
            └── configured boards
~~~

the same AROS binary should then observe:

~~~text
runtime probe
     │
     ▼
    true
     │
     ▼
shared classic ConfigChain
     │
     ▼
normal Autoconfig enumeration
~~~

No new AROS Expansion implementation should be required.

This is the primary extensibility benefit of the runtime design.

---

# 27. Conformance Requirements

The architecture is successful when all of the following hold.

## Single library

There is one `expansion.library`.

## Single classic implementation

Classic Amiga Autoconfig semantics exist in one source implementation.

## No source duplication

`m68k-emu68` does not contain copied classic Expansion source merely to obtain Amiga behavior.

## Runtime detection

The same `m68k-emu68` binary determines classic Expansion availability at runtime.

## No compile-time policy

Expansion availability is not controlled by an `EMU68_AUTOCONFIG` compile-time constant.

## No third implementation

There is no Emu68-specific `ConfigChain` algorithm or Autoconfig protocol.

## Correct empty-chain behavior

Expansion-domain presence is not inferred solely from whether a board currently responds.

## Platform isolation

Target-specific differences remain outside shared classic Expansion semantics.

## Bellatrix independence

Bellatrix may operate without a classic Expansion domain.

## Rigel independence

Expansion remains independent from Rigel.

## Future compatibility

Adding an Expansion provider to Bellatrix does not require another AROS Expansion implementation or another AROS binary.

---

# 28. Review Checklist

Every change related to this refactoring should answer:

1. Is this classic Amiga Expansion behavior?
2. If yes, why is it not in the shared classic implementation?
3. Is code being copied between `m68k-amiga` and `m68k-emu68`?
4. Is `m68k-emu68` implementing its own `ConfigChain` semantics?
5. Is the difference actually only platform detection?
6. Is the decision being made at runtime?
7. Would the same AROS binary work with Expansion present or absent?
8. Is board presence being confused with Expansion-domain presence?
9. Can the Expansion domain exist with zero boards?
10. Does a false probe prevent unsafe classic hardware accesses?
11. Does a true probe invoke the existing shared classic implementation?
12. Is target-specific RAM discovery being confused with Autoconfig semantics?
13. Is Bellatrix-specific knowledge leaking into the classic implementation?
14. Is Emu68 execution behavior defining a new Expansion protocol?
15. Is Expansion being incorrectly coupled to Rigel?
16. Could another M68K target reuse the same classic implementation?
17. Could Bellatrix later add Expansion without requiring another AROS build?
18. Has any compile-time `EMU68_AUTOCONFIG` policy been eliminated?

If these questions cannot be answered cleanly, the boundary should be reconsidered.

---

# 29. Target Architecture

The target architecture is:

~~~text
                         AROS
                           │
                           ▼
                   expansion.library
                           │
                           ▼
                  common library state
                           │
                           ▼
               platform capability check
                           │
                    ┌──────┴──────┐
                    │             │
               m68k-amiga    m68k-emu68
                    │             │
                 present       runtime
                               probe
                                  │
                              ┌───┴───┐
                              │       │
                           present   absent
                              │       │
                    ┌─────────┘       │
                    │                 │
                    ▼                 │
          shared classic Amiga       │
         Expansion / Autoconfig      │
             implementation          │
                    │                 │
                    ▼                 │
              M68K accesses           │
                    │                 │
                    ▼                 │
            Expansion hardware        │
                                      │
                                      ▼
                              no classic scan
~~~

The important property is:

~~~text
                     ONE LIBRARY

                         +

               ONE CLASSIC IMPLEMENTATION

                         +

             RUNTIME HARDWARE DETECTION
```

---

# 30. Required Migration

The migration from the current `m68k-emu68` arrangement should proceed as follows:

~~~text
1. Identify the classic Expansion code currently duplicated
   between m68k-amiga and m68k-emu68
        │
        ▼
2. Identify genuine platform-specific differences
        │
        ▼
3. Extract/share the classic implementation
        │
        ▼
4. Remove copied classic implementation
   from m68k-emu68
        │
        ▼
5. Remove compile-time EMU68_AUTOCONFIG policy
        │
        ▼
6. Add narrow m68k-emu68 runtime
   Expansion-domain probe
        │
        ▼
7. Invoke shared classic implementation
   only when runtime probe succeeds
        │
        ▼
8. Preserve platform-specific RAM/platform
   adaptation separately
        │
        ▼
9. Validate Bellatrix without Expansion
        │
        ▼
10. Validate runtime behavior against an
    Expansion-capable environment
```

No classic Autoconfig algorithm should be rewritten during this migration unless an independently demonstrated bug requires correction.

---

# 31. Definition of Done

This refactoring is complete when:

* only one `expansion.library` exists;
* classic Autoconfig semantics have one source implementation;
* `m68k-emu68` no longer carries a copied Amiga Expansion implementation;
* `m68k-emu68` detects classic Expansion availability at runtime;
* no compile-time `EMU68_AUTOCONFIG` decision remains;
* a machine without classic Expansion boots without invoking unsafe classic hardware enumeration;
* a machine with classic Expansion invokes the shared classic implementation;
* an Expansion domain with zero boards is distinguishable from absence of the domain;
* target-specific memory discovery remains separate from classic Autoconfig semantics;
* no Emu68-specific ConfigChain algorithm exists;
* no Bellatrix-specific Autoconfig protocol exists;
* Expansion remains independent from Rigel;
* the same AROS m68k-emu68 binary can support both machine configurations.

---

# 32. Final Recommendation

The current duplicated `m68k-emu68` Expansion implementation should not become the long-term architecture.

The desired transformation is:

~~~text
CURRENT

m68k-amiga
    │
    └── classic Expansion implementation

m68k-emu68
    │
    ├── copied classic Expansion implementation
    └── compile-time enable/disable


                    │
                    ▼


TARGET

              shared classic
          Expansion implementation
                    │
           ┌────────┴────────┐
           │                 │
      m68k-amiga        m68k-emu68
           │                 │
        present         runtime probe
                             │
                         ┌───┴───┐
                         │       │
                      present   absent
                         │       │
                         ▼       ▼
                      shared    skip
                      classic   scan
                      code
~~~

The governing rules are:

> There is one `expansion.library`.

> There is one implementation of classic Amiga Expansion and Autoconfig semantics.

> `m68k-emu68` must not contain a copy of that implementation.

> `m68k-emu68` must determine at runtime whether the currently running machine exposes the classic Expansion domain.

> Runtime detection is platform selection logic, not a third Expansion implementation.

> If classic Expansion is present, AROS invokes the shared classic implementation.

> If classic Expansion is absent, AROS skips classic hardware enumeration and continues normally.

> The presence of an Expansion domain and the presence of an Expansion board are separate questions.

> Bellatrix does not need to provide Expansion, but may add it later without requiring another AROS Expansion implementation.

> Rigel and Expansion remain independent machine domains.

The desired outcome is therefore not:

~~~text
create an Emu68 Expansion implementation
~~~

nor:

~~~text
maintain Amiga Expansion code
and an Emu68 copy
~~~

but:

~~~text
share the existing classic implementation

        +

detect at runtime whether
the machine can use it
~~~

This preserves classic AROS semantics, removes duplicated code, keeps Bellatrix free to expose or omit Expansion, and prevents `m68k-emu68` from becoming a third implementation of the Amiga Expansion architecture.
