// AI_context/sprint_23.md

# Sprint 23 — Chip RAM centralization pass + harness boot-path groundwork

## Description

This sprint focused on two closely related goals:

1. reduce the architectural spread of Chip RAM size knowledge
2. keep the harness boot investigation moving after confirming that `2 MB`
   Chip RAM changes timing but does not explain the missing boot screen by
   itself

The immediate trigger was a harness-side build failure after reducing visible
Chip RAM to `512K`, plus the broader realization that changing Chip RAM size
was still causing fallout in multiple unrelated places.

## Main conclusion

The previous model still leaked Chip RAM size assumptions into:

- Copper
- Agnus helpers
- the bus layer
- the harness Musashi backend
- unit tests
- debug logs and documentation

That made a simple configuration change feel much riskier than it should.

The corrective direction taken in this sprint was:

- keep one authoritative Chip RAM definition in `src/core/memory/memory.h`
- make consumers derive their behavior from that definition
- stop leaving functional code, tests, or docs with baked-in `2MB` assumptions

## What changed

### 1. Chip RAM size stayed reduced to `512K`

The visible Chip RAM configuration was left at:

- `BELLATRIX_CHIP_RAM_SIZE = 0x00080000`
- `BELLATRIX_CHIP_RAM_MASK = 0x0007FFFF`

This keeps the harness in the faster boot-timing mode that is more useful for
 diagnosis.

### 2. Copper build break fixed

`src/chipset/agnus/copper/copper_regs.c` had been switched to use
`BELLATRIX_CHIP_RAM_MASK` but was missing the header that defines it.

Fix:

- added `#include "memory/memory.h"`

This restored harness compilation.

### 3. Chip RAM constants were centralized further

In `src/core/memory/memory.h` the memory model now exposes:

- `BELLATRIX_CHIP_RAM_BASE`
- `BELLATRIX_CHIP_RAM_SIZE`
- `BELLATRIX_CHIP_RAM_END`
- `BELLATRIX_CHIP_RAM_MASK`
- `BELLATRIX_CHIP_BOOT_SIZE`
- `BELLATRIX_CHIP_BOOT_END`

And new helpers:

- `bellatrix_chip_addr_contains(addr)`
- `bellatrix_chip_addr_contains_range(addr, size)`

These are intended to remove repeated open-coded boundary logic from the rest
of the system.

### 4. Core memory decode and overlay logic now consume the central model

Files:

- `src/core/memory/memory_map.c`
- `src/core/memory/overlay.c`

Changes:

- region decode now uses central constants for Fast RAM, CIAs, custom,
  ROM, and Z2 config
- chip-RAM membership now uses `bellatrix_chip_addr_contains(...)`
- overlay comments were corrected to describe the configured model rather than
  a fixed `2 MiB` assumption
- the boot overlay window is now expressed in terms of
  `BELLATRIX_CHIP_BOOT_SIZE` / `BELLATRIX_CHIP_BOOT_END`

### 5. Bus and CPU-side checks stopped assuming `2MB`

Files:

- `src/core/bus.c`
- `src/cpu/bellatrix.c`

Changes:

- low Chip RAM membership checks now use the central helpers/constants
- ROM base/end references in the bus path now use the central ROM constants
- the CPU-side debug warning for `PC in Chip RAM` no longer uses the old
  `0x200000` threshold

### 6. Harness Musashi backend was aligned with the central memory model

File:

- `tools/harness/musashi_backend.c`

Changes:

- the main Chip RAM window checks now use
  `bellatrix_chip_addr_contains(...)`
- internal structure probing bounds now use
  `bellatrix_chip_addr_contains_range(...)`
- the low overlay redirect now uses `BELLATRIX_CHIP_BOOT_SIZE`
- stale comments that still described Chip RAM as
  `0x000000–0x1FFFFF` were corrected

This is important because the harness backend is exactly where configuration
drift becomes visible first.

### 7. Remaining obvious `2MB` consumers were cleaned up

Files:

- `src/chipset/agnus/blitter.c`
- `tests/unit/test_memory_map.c`
- `src/core/memory/Readme.md`

Changes:

- blitter pointer logging no longer masks with hardcoded `0x1FFFFF`
- unit tests now derive Chip RAM and Fast RAM behavior from the central
  constants instead of using hardcoded addresses that implied a fixed memory
  layout
- memory documentation now reflects the configured Chip RAM model more honestly

## Validation

Validated locally with:

- `cmake --build out/harness`
- `ctest --test-dir out/harness --output-on-failure -R "bellatrix_(unit_memory|unit_cia|unit_uart|integration_overlay)"`

Result:

- `100% tests passed`

An intermediate false negative was observed when `ctest` was launched in
parallel with an active rebuild; rerunning build and tests in sequence produced
the clean passing result above.

## Boot-path findings retained in this sprint

The harness investigation remains at the following state:

1. `2 MB` Chip RAM materially delays early Kickstart progress because the ROM
   spends much longer in the memory clear/probe loop
2. reducing visible Chip RAM to `512K` helps the harness reach the later boot
   path faster
3. even with that timing improvement, the missing boot screen is not explained
   by RAM size alone
4. by roughly the `40,000,000` cycle range, the harness already shows:
   - active Copper activity
   - `DMACON` enabled for the video path
   - `BPLCON0` programmed
   - bitplane pointers armed
   - `COLOR00` written

So the next investigation target is not “does Kickstart ever reach video
setup?” but rather:

- does Denise actually render the armed line state the way the ROM expects?
- or does the image disappear in the rendering / timing path after setup?

## Remaining follow-up

1. Continue from the harness video path, not from early memory probe again
2. Compare:
   - Agnus bitplane snapshot state
   - Denise line-entry conditions
   - framebuffer writes before flip
3. Determine whether the missing screen is caused by:
   - DIW/DDF interpretation
   - bitplane fetch/decode alignment
   - a Copper wake/timing issue
   - a Denise render-path suppression condition

## Files modified in this sprint

- `src/core/memory/memory.h`
- `src/core/memory/memory_map.c`
- `src/core/memory/overlay.c`
- `src/core/bus.c`
- `src/cpu/bellatrix.c`
- `src/chipset/agnus/copper/copper_regs.c`
- `src/chipset/agnus/blitter.c`
- `tools/harness/musashi_backend.c`
- `tests/unit/test_memory_map.c`
- `src/core/memory/Readme.md`

