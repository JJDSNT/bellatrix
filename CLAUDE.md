# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is Bellatrix

Bellatrix is a software Amiga chipset emulator that replaces the PiStorm hardware backend in Emu68. The goal is to run Amiga software (starting with Kickstart booting to "Happy Hand") entirely on a Raspberry Pi 3, with no Amiga hardware.

Emu68 handles M68K→AArch64 JIT translation; Bellatrix replaces only its bus backend. The JIT core is untouched.

## Repository Structure

```
bellatrix/
  emu68/                          # git submodule → michalsc/Emu68 (READ-ONLY upstream)
  src/variants/bellatrix/         # all new Bellatrix code
    bellatrix.h / bellatrix.c     # bus entry point: init + dispatch
    chipset/
      btrace.h / btrace.c         # bus trace — JSON Lines logging
      cia.h / cia.c               # (Phase 2+) CIA 8520 emulation
      agnus.h / agnus.c           # (Phase 3+) Agnus, copper, DMA
      ...
    platform/
      pal.h                       # Platform Abstraction Layer — only include
      raspi3/
        pal_debug.c               # PAL_Debug_* via Emu68's kprintf
        pal_ipl.c                 # PAL_IPL_Set/Clear → M68KState.INT
        pal_core.c                # stubs (Phase 3: dedicated ARM core)
  patches/
    0001-add-bellatrix-variant-cmake.patch   # CMakeLists.txt changes
    0002-add-bellatrix-bus-hook.patch        # vectors.c + start.c changes
  scripts/
    setup.sh    # apply patches + prerequisite check
    build.sh    # cmake + make with VARIANT=bellatrix
    flash.sh    # copy to SD card or upload via TFTP
  tools/
    btrace/
      btrace.py    # serial capture → JSON Lines
      analyze.py   # log analysis → unimplemented register report
  referencias/Emu68/   # reference copy of Emu68 source — READ ONLY, never modify
```

## Build Commands

```bash
# One-time setup (applies patches to emu68/ submodule)
./scripts/setup.sh

# Build
./scripts/build.sh

# Clean rebuild
./scripts/build.sh clean

# Flash to SD card
./scripts/flash.sh /media/user/BOOT

# Flash via TFTP
TFTP_HOST=192.168.1.10 ./scripts/flash.sh tftp
```

Prerequisites: `gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake` on Ubuntu.

## How Patches Work

The `patches/` directory holds minimal git-format-patch diffs for Emu68. Apply/update cycle:

```bash
# Apply (done by setup.sh)
cd emu68 && git apply ../patches/0001-... && git apply ../patches/0002-...

# Update after Emu68 upstream changes
cd emu68 && git pull origin master && git apply ../patches/...
# If a patch fails: fix conflict, regenerate patch with git format-patch
```

The patches are intentionally minimal — two files changed: `CMakeLists.txt` (variant registration) and `src/aarch64/vectors.c` + `src/aarch64/start.c` (bus hook + init call).

## Key Integration Points in Emu68

**Bus access hook** — `emu68/src/aarch64/vectors.c`:
- All M68K accesses to unmapped addresses (chipset, CIA, I/O) trigger AArch64 data abort
- `SYSHandler` → `SYSPageFaultWriteHandler`/`SYSPageFaultReadHandler` → `SYSWriteValToAddr`/`SYSReadValFromAddr`
- The patch adds `#elif defined(BELLATRIX)` between the PiStorm block and the bare block
- Bellatrix entry point: `bellatrix_bus_access(addr, value, size, dir)`

**Chip RAM** — Phase 1+: direct MMU mapping of `0x000000–0x1FFFFF` to a static ARM buffer (bypasses fault handler; uses `mmu_map()`).

**IPL injection** — `M68KState.INT.IPL` + `M68KState.INT.ARM`; accessed via `TPIDRRO_EL0` system register. `PAL_IPL_Set()` writes both and issues a DMB barrier.

**ABI note** — `ExecutionLoop.c` breaks the C ABI: M68K registers are pinned to ARM x13–x29, x12=JIT temp, x18=M68K PC. Any code in the hot path must respect these constraints.

## Development Cycle

```
build → flash → boot → capture (btrace) → analyze → implement → repeat
```

```bash
# Capture boot log
python3 tools/btrace/btrace.py --port /dev/ttyUSB0 --save boot.jsonl

# Analyze: show unimplemented registers
python3 tools/btrace/analyze.py --unimpl boot.jsonl

# Machine-readable report (for Claude Code consumption)
python3 tools/btrace/analyze.py --report boot.jsonl > report.json
```

Btrace verbosity is controlled at runtime by writing to address `0xDFFF00`:
- `0x0001` — only unimplemented (default)
- `0x0004` — chipset only
- `0xFFFF` — all accesses

## Session Continuity

- `AI_context/` — sprint-style session log. Read all files here before starting work.
- `docs/bellatrix_arquitetura.docx` — authoritative architecture document (Revision 3).
- `referencias/Emu68/` — reference copy of Emu68 for reading only.

## Implementation Phases

| Phase | Deliverable | Success Criterion |
|-------|-------------|-------------------|
| 0 | Infrastructure + btrace | Build succeeds; UART shows bus trace |
| 1 | Chip RAM MMU + ROM load | JIT executes first Kickstart instructions |
| 2 | CIA 8520 complete | Kickstart passes hardware detection |
| 3 | INTENA/INTREQ/VBL + dedicated core | Idle loop reached; copper list visible |
| 4 | Copper + Bitplanes + VC4 | Image on HDMI |
| 5 | Happy Hand | Animated cursor stable |
