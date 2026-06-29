# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What is Bellatrix

Bellatrix is a software Amiga chipset emulator that replaces the PiStorm hardware backend in Emu68. It runs entirely on a Raspberry Pi 3, with no Amiga hardware.

Kickstart boot, Workbench 1.3, Happy Hand (animated mouse pointer), and AROS desktop are all working. Current focus:
1. **Emu68 JIT integration** — performance, bus API stability, and quantum window correctness under the JIT backend.
2. **AROS newer ROM** — `aros.rom` (Jul/2025) boots to desktop. `new_aros.rom` (May/2026) stalls at boot screen without `HARNESS_MSGPORT_OWNER_FIX=1` workaround; clean fix pending (ISSUE-0020).

**ROM format**: AROS uses a 1MB concatenated ROM: `aros-ext.bin` (EXT, 512KB, maps to 0xE00000) followed by `aros-rom.bin` (main ROM, 512KB, maps to 0xF80000). Concatenate in that order to produce the ROM file expected by the harness.

Emu68 handles M68K→AArch64 JIT translation; Bellatrix replaces only its bus backend. The JIT core is untouched. There is also a Musashi (C M68K interpreter) backend used for development and the test harness.

## Build Commands

```bash
# One-time setup (applies patches to emu68/ submodule)
./scripts/setup.sh

# Four release configurations (CPU backend × core mode):

# Emu68 single-core  →  emu68/install-bellatrix-rigel/
BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 ./scripts/build.sh

# Musashi single-core  →  emu68/install-bellatrix-rigel-musashi/
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 ./scripts/build.sh

# Emu68 multi-core  →  emu68/install-bellatrix-rigel/
BELLATRIX_MULTICORE_BUILD=1 BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 ./scripts/build.sh

# Musashi multi-core  →  emu68/install-bellatrix-rigel-musashi/
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_MULTICORE_BUILD=1 BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 ./scripts/build.sh

# Append "clean" for a clean rebuild
```

**Build env vars:**
| Variable | Default | Description |
|---|---|---|
| `BELLATRIX_CPU_BACKEND` | `emu68` | `emu68` (JIT) or `musashi` (interpreter) |
| `BELLATRIX_MULTICORE_BUILD` | `0` | `1` = Core0 CPU / Core1 Chipset (Rigel) / Core3 IO |
| `BELLATRIX_BTSTACK` | `0` | Bluetooth HID host (BTStack) |
| `BELLATRIX_USBSTACK` | `0` | USB HID + mass storage (CherryUSB) |
| `BELLATRIX_LAUNCHER` | `1` | ADF/ISO selector UI |
| `BELLATRIX_OSD` | `1` | On-screen display overlay |

Prerequisites: `gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake` on Ubuntu.

```bash
# Flash to SD card
./scripts/flash.sh /media/user/BOOT

# Musashi harness (runs without hardware, for chipset development)
cd build_harness_rigel && cmake .. && make && ./bellatrix_harness
```

## Architectural Principles

The authoritative architecture is in `docs/timing_and_architetura.md` and `docs/roadmap.md`.

Key rules:
- **The chipset owns observable time** — not the machine, not Emu68.
- **Paula owns INTREQ/INTENA** — CIA and Agnus raise events; Paula consolidates.
- **DMA belongs to Agnus** — arbitration, copper, blitter, beam.
- **Denise is an explicit instance** — not a singleton global.
- **Copper is subordinate to Agnus** — not an independent subsystem.

Component composition:
```
BellatrixMachine
 ├── cpu   (struct M68KState *)
 ├── cia_a (CIA)
 ├── cia_b (CIA)
 ├── paula (Paula)
 ├── agnus (Agnus / AgnusState)
 │    └── copper, blitter, dma
 └── denise (Denise)
```

## Repository Structure

```
bellatrix/
  emu68/                    # git submodule → michalsc/Emu68 (READ-ONLY upstream)
  external/                 # git submodules: btstack, cherryusb, musashi, rigel, aros
  src/
    cpu/
      bellatrix.h/.c        # Emu68 bus entry point: init + bus dispatch
    core/
      machine.h/.c          # BellatrixMachine — integration and bus protocol
      btrace.h/.c           # bus trace — JSON Lines logging
    chipset/
      cia/cia.h/.c          # CIA 8520
      agnus/agnus.h/.c      # Agnus beam, DMA, copper, blitter
      agnus/copper.h/.c
      agnus/blitter.h/.c
      denise/denise.h/.c    # Bitplane render
      paula/paula.h/.c      # IRQ consolidation, serial, audio
    io/
      bluetooth/            # BTStack HID host (bt_host, bt_scan, bt_pairs,
                            #   bt_hid, bt_link_key_db_sd, bt_hal_raspi3)
      usb/                  # CherryUSB HID + MSC
      hid/hid_amiga_map.h  # Shared HID→Amiga rawkey table (USB + BT)
    launcher/               # Bare-metal ADF/ISO selector UI (FAT32 + VC4)
    storage/
      fat/fat32.h/.c        # FAT32 reader/writer (in-place overwrite only)
      sdcard/bcm_emmc.h/.c  # EMMC/SD card driver
      iso/                  # ISO 9660 reader
    host/
      pal.h                 # Platform Abstraction Layer
      raspi3/               # PAL implementations: IPL, debug, timer, core
  patches/                  # git-format-patch diffs applied to emu68/ submodule
    0001 – bellatrix variant cmake
    0002 – bus hook (vectors.c + start.c)
    0003 – ExecutionLoop cycle ownership
    0004 – CherryUSB DWC2 host
    0005 – BTStack BCM bare-metal init
    0006 – Musashi instruction hook
  tools/harness/            # Musashi-based chipset test harness (no hardware needed)
  referencias/Emu68/        # READ-ONLY reference copy of Emu68 source
  docs/
    roadmap.md
    timing_and_architetura.md
  AI_context/               # Sprint-style session logs — read before starting work
```

## Key Integration Points in Emu68

**Bus access hook** — `emu68/src/aarch64/vectors.c`:
- M68K accesses to unmapped addresses trigger AArch64 data abort
- `SYSHandler` → `SYSPageFaultWriteHandler/ReadHandler` → `SYSWriteValToAddr/SYSReadValFromAddr`
- Patch adds `#elif defined(BELLATRIX)` block; entry point: `bellatrix_bus_access(addr, value, size, dir)`

**Chip RAM** — direct MMU mapping `0x000000–0x1FFFFF` to static ARM buffer via `mmu_map()`, bypassing the fault handler.

**IPL injection** — `M68KState.INT.IPL` + `M68KState.INT.ARM` via `TPIDRRO_EL0`. `PAL_IPL_Set()` writes both + DMB barrier.

**ABI constraint** — `ExecutionLoop.c` pins M68K registers to ARM x13–x29, x12=JIT temp, x18=M68K PC. Never touch these in hot-path code.

## Bluetooth Subsystem

BTStack HID host running on the BCM43430A1 chip (Pi 3B onboard BT).

**Initialization chain:** `bt_host_init()` → power-cycle BCM via GPIO → PatchRAM download → H4 UART at 115200 → `btstack_run_loop_embedded_execute_once()` in polling loop.

**Pairing flow:** `bt_scan_screen()` (launcher) → inquiry → user selects device → `bt_pairs_add()` → saved to `BTPAIRS.TXT` on SD. Link keys saved to `BTKEYS.TXT` via `bt_link_key_db_sd` (implements `btstack_link_key_db_t`).

**Connection flow (post-launcher):** `bt_host_connect_pairs()` → `hid_host_connect()` per saved pair → `hid_packet_handler()` routes HID reports to `bt_hid_handle_keyboard/mouse/joystick_report()`.

**RX architecture:** `bt_hal_raspi3_drain_fifo()` drains PL011 FIFO into a 4KB software ring buffer (safe to call from any context). `bt_hal_raspi3_poll_uart()` consumes the ring and feeds the H4 parser.

**SD files** (must exist on SD before first boot; shipped as placeholders in install dir):
- `BTPAIRS.TXT` — paired device list (addr, type, name)
- `BTKEYS.TXT` — BT Classic link keys for automatic reconnection
- `BTSCAN.TXT` — diagnostic log written after each scan + connection attempt

## How Patches Work

```bash
# Applied by setup.sh — do not apply manually
cd emu68 && git apply ../patches/0001-...

# Regenerate after upstream changes
cd emu68 && git pull origin master
git apply ../patches/NNNN-...
# On conflict: fix, then git format-patch HEAD~1 > ../patches/NNNN-...
```

Patches are kept minimal. The `emu68/` and `external/cherryusb` submodule files tracked by patches are set with `git update-index --assume-unchanged` to suppress dirty status.

## Development Cycle

```
build → flash → boot → read BTSCAN.TXT / serial → fix → repeat
```

```bash
# Capture boot log via serial (btrace)
python3 tools/btrace/btrace.py --port /dev/ttyUSB0 --save boot.jsonl
python3 tools/btrace/analyze.py --unimpl boot.jsonl
python3 tools/btrace/analyze.py --report boot.jsonl > report.json
```

Btrace verbosity (write to address `0xDFFF00` at runtime):
- `0x0001` — unimplemented only (default)
- `0x0004` — chipset only
- `0xFFFF` — all accesses

## Session Continuity

- `AI_context/` — structured project memory (SDLC). Read open issues (`issues/`) and
  consolidated knowledge (`consolidated/`) before starting work. See `AI_context/README.md`.
- `docs/future_roadmap.md` — long-term architectural direction.
- `docs/runtime_and_timing.md` — timing model and component contracts.
- `docs/rigel_gap_analysis.md` — Rigel integration status and remaining gaps.
- `referencias/Emu68/` — reference Emu68 source; READ ONLY, never modify.

## Implementation Phases

| Phase | Deliverable | Status |
|---|---|---|
| 0 | Infrastructure + btrace | ✅ Done |
| 1 | Chip RAM MMU + ROM load | ✅ Done |
| 2 | CIA 8520 complete | ✅ Done |
| 3 | INTENA/INTREQ/VBL + dedicated core | ✅ Done |
| 4 | Copper + Bitplanes + VC4 | ✅ Done |
| 5 | Happy Hand | ✅ Done |
| 6 | Emu68 JIT integration | 🔄 In progress |
| 7 | AROS desktop | 🔄 AROS renders screen; Workbench full load pending |
