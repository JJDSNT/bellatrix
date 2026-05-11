# `docs/testing/runtime_core_testing.md`

````md id="2m1b4h"
# Bellatrix — Runtime Core Testing Strategy

## Purpose

Define the canonical testing strategy for the Bellatrix multicore runtime.

This document defines:

- per-core validation
- cross-core contract validation
- ownership validation
- runtime synchronization validation
- deterministic boot testing
- clean-reset testing flow
- future regression strategy

This document is the source of truth for:

```text
how Bellatrix runtime correctness is validated
````

---

# 1. Architectural Principle

The Bellatrix runtime is divided into explicit execution domains.

Because of this:

> correctness must be validated per domain and across domain boundaries.

Testing therefore must validate:

* local domain behavior
* cross-core event propagation
* ownership correctness
* runtime causality
* synchronization consistency

---

# 2. Why Runtime Core Testing Exists

Historically, emulator testing focused on:

* CPU instruction correctness
* ROM boot success
* visual correctness

Bellatrix now requires additional validation because:

* runtime is multicore
* ownership is distributed
* timing is domain-based
* events propagate asynchronously
* runtime synchronization affects behavior

---

# 3. Testing Layers

Bellatrix runtime testing is divided into:

| Layer             | Goal                               |
| ----------------- | ---------------------------------- |
| Core Tests        | validate isolated domain behavior  |
| Contract Tests    | validate inter-core propagation    |
| Ownership Tests   | validate architectural correctness |
| Integration Tests | validate complete machine behavior |
| Boot Tests        | validate real runtime startup      |
| Regression Tests  | prevent architectural regressions  |

---

# 4. Runtime Boot Test Flow

## Recommended Flow

After Raspberry Pi boot:

```text id="xk0y4q"
boot runtime
    ↓
execute runtime validation suite
    ↓
collect results/logs
    ↓
hard-reset Bellatrix machine state
    ↓
start clean execution session
```

---

# 5. Why Reset After Tests

Runtime tests may:

* alter timers
* leave pending IRQs
* modify DMA state
* change memory contents
* leave cores desynchronized

Because Bellatrix is temporal and stateful:

> test residue must never contaminate normal execution.

Therefore:

```text id="8zh3x5"
runtime validation should be followed by full machine reset
```

before production execution.

---

# 6. Full Reset Requirements

A proper runtime reset must reset:

* CPU state
* IPL
* runtime synchronization
* DMA state
* Copper state
* beam state
* CIA timers
* Paula streams
* interrupt state
* memory overlays
* runtime mailboxes/events
* future logical clocks

---

# 7. Core-Level Tests

Each runtime domain should have isolated validation tests.

---

# 8. Core 3 — IO/CIA Tests

## Responsibilities

Core 3 owns:

* CIA A/B
* timers
* TOD
* keyboard protocol
* UART host integration
* classic IO

---

## Example Tests

### CIA Timer Underflow

Input:

```text id="x91b4r"
ticks = N
CIA timer loaded
CRA enabled
```

Expected Output:

```text id="l8c8ta"
timer decrements
underflow occurs
ICR bit set
IRQ event generated
```

---

### TOD Pulse

Input:

```text id="d0w87v"
HSYNC pulses
```

Expected Output:

```text id="2jdf0l"
TOD increments coherently
```

---

### Floppy Line Update

Input:

```text id="y8wz1k"
CIAB PRB write
/SEL0 active
/MTR active
```

Expected Output:

```text id="f0rj3x"
floppy lines updated
motor state updated
select state updated
```

---

# 9. Core 2 — Paula Tests

## Responsibilities

Core 2 owns:

* Paula audio
* Paula serial
* Paula disk
* interrupt consolidation
* IPL publication

---

## Example Tests

### IRQ Consolidation

Input:

```text id="xj2c7m"
CIA IRQ event
INTENA enabled
```

Expected Output:

```text id="jsk0fz"
INTREQ updated
pending IRQ calculated
IPL derived
```

---

### Disk DMA

Input:

```text id="zwu5lh"
DSKLEN enabled
MFM words available
```

Expected Output:

```text id="5i92s4"
DSKBYTR updated
DMA request generated
sync detection coherent
```

---

### Serial Timing

Input:

```text id="2vwbry"
SERDAT loaded
serial ticks advanced
```

Expected Output:

```text id="e3v2vv"
SERDATR updated
IRQ generated if appropriate
```

---

# 10. Core 1 — Video/DMA Tests

## Responsibilities

Core 1 owns:

* Agnus
* Denise
* raster
* DMA
* copper
* blitter
* bitplanes

---

## Example Tests

### DMA Arbitration

Input:

```text id="rm7y9g"
DMA request active
DMACON enabled
beam at valid slot
```

Expected Output:

```text id="g7rqbe"
DMA grant occurs
slot ownership coherent
```

---

### Copper WAIT/MOVE

Input:

```text id="1pr4kp"
beam reaches WAIT condition
```

Expected Output:

```text id="3i0nzd"
MOVE executes immediately afterward
register updated coherently
```

---

### VBL Generation

Input:

```text id="m9rqci"
beam reaches VBL region
```

Expected Output:

```text id="u3u1o7"
VBL event generated
```

---

# 11. Core 0 — Execution Tests

## Responsibilities

Core 0 owns:

* Emu68 execution
* CPU state
* interrupt consumption
* memory execution

---

## Example Tests

### IRQ Acceptance

Input:

```text id="6vtg7m"
IPL rises above CPU mask
```

Expected Output:

```text id="0q6rxy"
interrupt exception taken
vector resolved correctly
```

---

### MMIO Read/Write

Input:

```text id="1nq5o5"
MMIO request issued
```

Expected Output:

```text id="t2t6ru"
runtime/bus access path respected
```

---

# 12. Cross-Core Contract Tests

These tests validate event propagation across domains.

---

# 13. CIA → Paula → CPU

## Flow

```text id="0cwyca"
Core 3 CIA
    ↓
Core 2 Paula
    ↓
Core 0 CPU
```

---

## Expected Behavior

```text id="4pyqcl"
CIA timer underflow
    ↓
ICR set
    ↓
Paula consolidates INTREQ
    ↓
IPL derived
    ↓
CPU accepts interrupt
```

---

# 14. Floppy → Paula → DMA

## Flow

```text id="2ozx0v"
Core 3 floppy lines
    ↓
Core 2 Paula disk
    ↓
Core 1 DMA arbitration
```

---

## Expected Behavior

```text id="jlwm9p"
drive selected
disk DMA starts
DMA request issued
Agnus grants slot
```

---

# 15. Ownership Tests

Ownership tests validate architecture rules.

---

## Examples

### Forbidden Calls

Core 3 MUST NOT call:

```text id="jlwm8p"
paula_interrupt_update()
```

Core 1 MUST NOT:

```text id="jlwm7p"
derive IPL
```

CPU MUST NOT:

```text id="jlwm6p"
modify CIA internal timers directly
```

---

# 16. Static Validation

Some ownership tests may be static.

Examples:

```bash id="6ek0w5"
grep -R "paula_interrupt_update" src/runtime/core_io.c
```

Expected result:

```text id="jlwm5p"
no matches
```

---

# 17. Runtime Logging

Per-core runtime logs are strongly recommended.

---

## Recommended Tags

| Domain     | Tag           |
| ---------- | ------------- |
| Core 0     | [CORE0-CPU]   |
| Core 1     | [CORE1-GFX]   |
| Core 2     | [CORE2-PAULA] |
| Core 3     | [CORE3-IO]    |
| Cross-core | [XCORE-*]     |

---

# 18. Deterministic Testing

Whenever possible:

* fixed tick counts
* deterministic event ordering
* reproducible DMA timing
* stable synchronization

should be preferred.

---

# 19. Future Runtime Testing

Future runtime testing may include:

```text id="jlwm4p"
mailbox validation
logical clock validation
event timestamp validation
runtime causality verification
lock-free synchronization validation
```

---

# 20. Regression Philosophy

Every architectural bug should produce:

* a reproducible runtime test
* a regression case
* a domain ownership validation

This prevents:

* ownership drift
* synchronization regressions
* temporal incoherence

---

# 21. Runtime Philosophy

Bellatrix correctness is NOT merely:

```text id="jlwm3p"
the ROM boots
```

Bellatrix correctness means:

```text id="jlwm2p"
runtime domains evolve coherently
ownership is respected
causality remains stable
```

---

# 22. Final Principle

> Runtime testing validates not only execution correctness,
> but architectural coherence.

```
```
