# `docs/testing/regression_and_validation_strategy.md`

````md id="0v8ehv"
# Bellatrix — Regression Prevention and Validation Strategy

## Purpose

Define the canonical strategy for preventing architectural regressions in Bellatrix.

This document defines:

- regression prevention philosophy
- validation layers
- ownership validation
- runtime contract validation
- boot milestone validation
- architectural invariants
- runtime evolution safety rules

This document is the source of truth for:

```text
how Bellatrix evolves without losing architectural coherence
````

---

# 1. Historical Context

Bellatrix has already experienced strong regressions caused by:

* ownership drift
* implicit coupling
* hidden synchronization
* mixed responsibilities
* temporal side effects
* architectural ambiguity

Examples included:

* IRQ ownership drift
* MMIO causing implicit timing
* runtime domains executing foreign responsibilities
* DMA behavior changing unexpectedly
* serial regressions after runtime changes
* copper timing regressions
* memory decode duplication

---

# 2. Core Principle

> Architecture must become enforceable, not merely descriptive.

This means:

* ownership must be testable
* runtime behavior must be observable
* synchronization must be verifiable
* contracts must be validated automatically

---

# 3. Bellatrix Philosophy

Bellatrix correctness is NOT merely:

```text id="9z1m4u"
the ROM boots
```

Bellatrix correctness means:

```text id="9g8jcc"
ownership is respected
runtime causality is coherent
domains evolve correctly
cross-core propagation behaves consistently
```

---

# 4. Validation Layers

Bellatrix validation is divided into:

| Layer             | Goal                               |
| ----------------- | ---------------------------------- |
| Unit Tests        | validate isolated domain behavior  |
| Contract Tests    | validate cross-core propagation    |
| Ownership Tests   | validate architecture rules        |
| Integration Tests | validate combined runtime behavior |
| Boot Tests        | validate complete runtime startup  |
| Regression Tests  | prevent historical regressions     |

---

# 5. Mandatory Validation Rule

## Rule

> No runtime change is complete without validation.

Every significant runtime change MUST include:

1. domain validation
2. cross-core validation
3. ownership validation
4. boot smoke validation
5. regression comparison logs

---

# 6. Architectural Invariants

These invariants MUST remain true.

---

# 7. Interrupt Ownership

## Invariant

```text id="jlwm1x"
Paula owns INTREQ / INTENA / IPL
```

Therefore:

* CIA never derives IPL
* Agnus never consolidates IRQ
* Core 3 never publishes IPL
* Core 1 never manipulates INTREQ directly

---

# 8. DMA Ownership

## Invariant

```text id="jlwm2x"
Agnus owns DMA arbitration
```

Therefore:

* DMA timing must not exist outside runtime ownership
* DMA cannot become isolated side effects
* CPU timing cannot bypass DMA visibility

---

# 9. Memory Ownership

## Invariant

```text id="jlwm3x"
memory_map is the source of truth
```

Therefore:

* decode logic must not be duplicated
* ownership cannot be inferred from mappings alone
* Emu68 mappings do not define semantics

---

# 10. Runtime Ownership

## Invariant

Each runtime core owns explicit responsibilities only.

---

## Core 0

Owns:

* execution
* CPU runtime
* Emu68 integration

Must NOT own:

* DMA arbitration
* IRQ consolidation
* CIA timing

---

## Core 1

Owns:

* Agnus
* Denise
* raster
* DMA
* copper
* blitter

Must NOT own:

* IPL derivation
* Paula interrupt consolidation

---

## Core 2

Owns:

* Paula
* audio
* serial
* disk Paula
* INTREQ
* IPL publication

Must NOT own:

* CIA timer evolution
* raster ownership

---

## Core 3

Owns:

* CIA
* timers
* keyboard protocol
* UART host
* classic IO

Must NOT own:

* Paula interrupt consolidation
* IPL derivation
* DMA arbitration

---

# 11. MMIO Invariant

## Rule

```text id="jlwm4x"
MMIO changes state.
Runtime evolves time.
```

MMIO MUST NOT:

* evolve raster
* advance timers directly
* execute temporal loops
* bypass runtime synchronization

---

# 12. Regression Prevention Strategy

Bellatrix uses:

```text id="jlwm5x"
architectural contracts
+
runtime validation
+
deterministic logging
+
milestone boot testing
```

to prevent regressions.

---

# 13. Runtime Milestone Ladder

Bellatrix runtime evolution should be validated progressively.

---

## Level 1 — Runtime Startup

Expected:

```text id="jlwm6x"
Emu68 runtime boots
cores initialize
runtime starts coherently
```

---

## Level 2 — Memory Coherence

Expected:

```text id="jlwm7x"
overlay works
ROM visible
Fast RAM visible
memory probing coherent
```

---

## Level 3 — Basic Interrupt Path

Expected:

```text id="jlwm8x"
CIA timer
    ↓
Paula INTREQ
    ↓
IPL
    ↓
CPU interrupt
```

---

## Level 4 — Serial Runtime

Expected:

```text id="jlwm9x"
SERDAT/SERDATR coherent
TX/RX timing coherent
polling loops terminate
```

---

## Level 5 — Raster Runtime

Expected:

```text id="jlwm0x"
beam progression coherent
VBL generated
DMA visible
```

---

## Level 6 — Copper Runtime

Expected:

```text id="jlwmax"
WAIT/MOVE ordering coherent
register updates visible
```

---

## Level 7 — Floppy Runtime

Expected:

```text id="jlwmbx"
drive select
motor state
ready/chg behavior
disk DMA startup
```

---

## Level 8 — Full Compatibility Milestones

Examples:

* Happy Hand
* Kickstart menu
* AROS boot progression

---

# 14. Regression Rule

## Principle

> Every fixed bug should produce a permanent regression test.

Examples:

* IRQ regression → IRQ contract test
* DMA regression → DMA timing test
* serial regression → serial progression test
* ownership regression → static ownership validation

---

# 15. Static Ownership Validation

Some architectural rules should be validated statically.

---

## Example

```bash id="xxtkt6"
grep -R "paula_interrupt_update" src/runtime/core_io.c
```

Expected:

```text id="jlwmcx"
no matches
```

---

# 16. Runtime Logging

Per-core logging is strongly recommended.

---

## Suggested Tags

| Domain     | Tag           |
| ---------- | ------------- |
| Core 0     | [CORE0-CPU]   |
| Core 1     | [CORE1-GFX]   |
| Core 2     | [CORE2-PAULA] |
| Core 3     | [CORE3-IO]    |
| Cross-core | [XCORE-*]     |

---

# 17. Golden Reference Logs

Bellatrix should maintain:

```text id="jlwmdx"
golden reference logs
```

for critical milestones.

Examples:

* boot startup
* interrupt flow
* serial startup
* copper execution
* DMA startup

Future regressions can then be compared directly.

---

# 18. Deterministic Validation

Whenever possible:

* fixed tick progression
* deterministic ordering
* reproducible runtime state
* deterministic event ordering

should be preferred.

---

# 19. Runtime Reset Strategy

Recommended startup sequence:

```text id="jlwmex"
boot runtime
    ↓
run validation suite
    ↓
collect logs/results
    ↓
hard reset machine/runtime
    ↓
start clean execution session
```

This prevents:

* stale IRQs
* DMA residue
* runtime desynchronization
* test contamination

---

# 20. Runtime Philosophy

Bellatrix is NOT evolving toward:

```text id="jlwmfx"
a fragile pile of timing hacks
```

Bellatrix is evolving toward:

```text id="jlwmgx"
a coherent runtime architecture
with explicit ownership and observable causality
```

---

# 21. Final Principle

> Regressions are prevented not only through testing,
> but through enforceable architectural boundaries.

```
```
