# Bellatrix

**Bellatrix** is a bare-metal Amiga machine emulator for the Raspberry Pi 3B. It integrates the [Emu68](https://github.com/michalsc/Emu68) M68K JIT engine with the [Rigel](https://github.com/jfdelnero/Rigel) chipset library and a full suite of bare-metal IO subsystems, producing a complete Amiga-compatible machine that runs directly on the Pi hardware — no OS, no Linux, no hypervisor.

---

## Architecture

Bellatrix is the machine layer that integrates three main pieces:

```
┌─────────────────────────────────────────────────┐
│               Amiga software                     │
│          (Kickstart / Workbench / demos)         │
└─────────────────────┬───────────────────────────┘
                      │  M68K instructions
┌─────────────────────▼───────────────────────────┐
│                   Emu68                          │
│          M68K → AArch64 JIT + MMU               │
└─────────────────────┬───────────────────────────┘
                      │  bus accesses (data abort hook)
┌─────────────────────▼───────────────────────────┐
│                 Bellatrix                        │
│   machine runtime · bus protocol · IO · timing  │
│                      │                          │
│          ┌───────────▼───────────┐              │
│          │         Rigel         │              │
│          │  Amiga chipset (CIA   │              │
│          │  Agnus·Paula·Denise)  │              │
│          └───────────────────────┘              │
└─────────────────────────────────────────────────┘
              Raspberry Pi 3B bare metal
```

**Emu68** provides the M68K execution engine. All accesses to chipset and CIA addresses trigger an AArch64 data abort, which Emu68 routes to Bellatrix's bus hook. Bellatrix decodes the address, dispatches to Rigel (chipset) or its own IO subsystems, and returns — the M68K software never knows it is running on ARM.

**Rigel** is the chipset library: it owns CIA, Agnus, Paula, and Denise behavior, timing, DMA arbitration, and interrupt consolidation. Bellatrix forwards chipset bus accesses to Rigel and drives its temporal evolution.

**Bellatrix** owns everything else: machine composition, the bus protocol, chip RAM MMU mapping, the launcher UI, Bluetooth and USB HID input, SD card storage, and the multicore runtime that coordinates all domains.

---

## Current focus

- **Emu68 integration** — full JIT backend running with the Rigel chipset
- **AROS boot** — AROS reaches the kitty screen but does not complete the boot sequence

---

## Features

- **Bare-metal** — boots directly on Pi 3B hardware (BCM2837, AArch64). No Linux.
- **Launcher UI** — framebuffer menu for selecting ADF (floppy) and ISO (CD-ROM) images from SD card or USB drive.
- **Bluetooth HID** — pairs keyboards, mice, and gamepads via the Pi 3B's onboard BCM43430A1. Devices reconnect automatically across reboots (link keys persisted to SD).
- **USB HID + MSC** — USB keyboard, mouse, gamepad, and USB drives via CherryUSB.
- **Bus trace** — chipset register accesses logged as JSON Lines over serial for offline analysis.
- **Musashi harness** — an interpreter-based development backend that runs the same chipset code without hardware.

---

## Hardware

- Raspberry Pi 3B (BCM2837, onboard BCM43430A1 Bluetooth/WiFi)
- MicroSD card with FAT32 boot partition
- Kickstart ROM image (not included)
- Optional: USB keyboard/mouse, Bluetooth keyboard/mouse, USB drive

The Pi 3B+ works but is not the primary target. Pi 4 is not supported.

---

## Building

### Prerequisites

```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu cmake
```

### One-time setup

```bash
git clone --recurse-submodules https://github.com/YOUR_USERNAME/bellatrix
cd bellatrix
./scripts/setup.sh        # applies patches to emu68/ submodule
```

### Release configurations

There are four release builds, defined by two orthogonal axes:

| | Single-core | Multi-core |
|---|---|---|
| **Emu68** (JIT) | `BELLATRIX_CPU_BACKEND=emu68` | `+ BELLATRIX_MULTICORE_BUILD=1` |
| **Musashi** (interpreter) | `BELLATRIX_CPU_BACKEND=musashi` | `+ BELLATRIX_MULTICORE_BUILD=1` |

**Single-core:** all components (CPU, chipset, IO) run on Core 0. Simpler, no synchronization overhead, currently the most stable.

**Multi-core:** Core 0 = CPU (Emu68 JIT), Core 1 = full chipset (Rigel: CIA+Agnus+Paula+Denise), Core 3 = IO (USB + Bluetooth). Better timing isolation between CPU and chipset.

**Emu68:** full M68K→AArch64 JIT. Fast, close to real hardware speed.

**Musashi:** M68K interpreter in C. Slower, but easier to instrument and iterate on without reflashing.

### Build

```bash
# Emu68 single-core (JIT, production target)
BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 \
  ./scripts/build.sh
# → emu68/install-bellatrix-rigel/

# Musashi single-core (interpreter, active development)
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 \
  ./scripts/build.sh
# → emu68/install-bellatrix-rigel-musashi/

# Emu68 multi-core
BELLATRIX_MULTICORE_BUILD=1 BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 \
  ./scripts/build.sh

# Musashi multi-core
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_MULTICORE_BUILD=1 \
  BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 ./scripts/build.sh

# Clean rebuild: append "clean"
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 \
  ./scripts/build.sh clean
```

**All build options:**

| Variable | Default | Description |
|---|---|---|
| `BELLATRIX_CPU_BACKEND` | `emu68` | `emu68` (JIT) or `musashi` (interpreter) |
| `BELLATRIX_MULTICORE_BUILD` | `0` | `1` = Core0 CPU / Core1 Chipset (Rigel) / Core3 IO |
| `BELLATRIX_BTSTACK` | `0` | Bluetooth HID host (BTStack) |
| `BELLATRIX_USBSTACK` | `0` | USB HID + mass storage (CherryUSB) |
| `BELLATRIX_LAUNCHER` | `1` | ADF/ISO selector UI |
| `BELLATRIX_OSD` | `1` | On-screen display overlay |

### Flash to SD

Copy the install directory to the FAT32 boot partition:

```bash
cp emu68/install-bellatrix-rigel-musashi/* /media/user/BOOT/
```

The SD card must also contain three pre-sized placeholder files (shipped in the install directory). The bare-metal FAT32 writer can only overwrite files in place — it never allocates new clusters — so these must exist before the first boot:

- `BTPAIRS.TXT` — saved paired Bluetooth device list
- `BTKEYS.TXT` — Bluetooth Classic link keys (for automatic reconnection)
- `BTSCAN.TXT` — diagnostic log written on each boot

---

## Bluetooth pairing

On first boot, the launcher shows a Bluetooth scan screen. Put your keyboard or mouse into pairing mode and press ENTER to pair. The device address and link key are saved to the SD card.

On subsequent boots, paired devices reconnect automatically — no need to press any button.

---

## Diagnostics

Chipset register accesses can be logged over the Pi's serial port (GPIO 14/15, 115200 8N1):

```bash
python3 tools/btrace/btrace.py --port /dev/ttyUSB0 --save boot.jsonl
python3 tools/btrace/analyze.py --unimpl boot.jsonl
```

Bluetooth diagnostics are written to `BTSCAN.TXT` on the SD card after each scan and connection attempt.

---

## Repository layout

```
emu68/          Emu68 submodule — M68K JIT (READ-ONLY upstream)
external/       rigel (chipset), btstack, cherryusb, musashi, aros (submodules)
src/
  cpu/          bus entry point — bridges Emu68 data abort to Bellatrix
  core/         BellatrixMachine, bus protocol, bus trace
  chipset/      thin wrappers / legacy chipset code (transitioning to Rigel)
  io/
    bluetooth/  BTStack HID host (scan, pairs, link keys, HAL, HID dispatch)
    usb/        CherryUSB HID + MSC
    hid/        shared HID→Amiga rawkey map (used by both BT and USB)
  launcher/     bare-metal ADF/ISO selector UI (VC4 framebuffer)
  storage/      FAT32, EMMC/SD card, ISO 9660
  host/         Platform Abstraction Layer (IPL, timer, debug, core stubs)
patches/        minimal diffs applied to emu68/ (6 patches)
tools/harness/  Musashi-based chipset test harness — runs without hardware
docs/           architecture specifications
referencias/    READ-ONLY reference copy of Emu68 source
```

---

## Credits

- [Emu68](https://github.com/michalsc/Emu68) by Michal Schulz — M68K→AArch64 JIT engine
- [Rigel](https://github.com/jfdelnero/Rigel) — Amiga chipset library (CIA, Agnus, Paula, Denise)
- [BTStack](https://github.com/bluekitchen/btstack) — Bluetooth host stack
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) — USB host stack
- [Musashi](https://github.com/kstenerud/musashi) — M68K interpreter (development backend)
