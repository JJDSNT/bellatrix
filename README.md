# Bellatrix

**Bellatrix** is a software Amiga chipset emulator for the Raspberry Pi 3B. It replaces the PiStorm hardware backend in [Emu68](https://github.com/michalsc/Emu68), making it possible to run Amiga software entirely in software — no Amiga hardware required.

The goal: boot Kickstart to the "Happy Hand" Workbench screen on bare metal, using only a Raspberry Pi 3B and an SD card.

---

## How it works

Emu68 is a high-performance M68K→AArch64 JIT translator originally designed to run inside a real Amiga (via PiStorm). Bellatrix replaces only the bus backend — the JIT core is completely untouched.

```
┌─────────────────────────────────────────────┐
│                  Amiga software              │
│           (Kickstart / Workbench)            │
└──────────────────────┬──────────────────────┘
                       │  M68K instructions
┌──────────────────────▼──────────────────────┐
│                    Emu68                     │
│         M68K → AArch64 JIT + MMU            │
└──────────────────────┬──────────────────────┘
                       │  bus accesses (data abort → hook)
┌──────────────────────▼──────────────────────┐
│                  Bellatrix                   │
│   CIA · Agnus · Paula · Denise · DMA · IO   │
└─────────────────────────────────────────────┘
              Raspberry Pi 3B bare metal
```

All M68K accesses to chipset/CIA addresses trigger an AArch64 data abort, which Emu68 routes to `bellatrix_bus_access()`. Bellatrix handles the register, updates chipset state, and returns — the CPU never knows it's running on ARM.

---

## Status

Active development. Current progress:

| Phase | Goal | Status |
|---|---|---|
| 0 | Infrastructure + bus trace | ✅ done |
| 1 | Chip RAM + ROM load | ✅ done |
| 2 | CIA 8520 complete | ✅ done |
| 3 | INTENA/INTREQ/VBL | 🔄 in progress |
| 4 | Copper + Bitplanes + HDMI | ⏳ next |
| 5 | Happy Hand | ⏳ goal |

---

## Features

- **Bare-metal execution** — no OS, no Linux, no hypervisor. Emu68 boots directly on the Pi 3B hardware.
- **Launcher UI** — framebuffer menu for selecting ADF (floppy) and ISO (CD-ROM) images from SD card or USB drive.
- **Bluetooth HID host** — pairs keyboards, mice, and gamepads via the Pi 3B's onboard BCM43430A1. Devices reconnect automatically across reboots (link keys persisted to SD).
- **USB HID + MSC** — USB keyboard, mouse, gamepad input via CherryUSB. USB drives for ADF/ISO loading.
- **Bus trace** — every chipset register access can be logged as JSON Lines over serial and analyzed offline.
- **Musashi harness** — a C M68K interpreter backend for chipset development without hardware.

---

## Hardware

- Raspberry Pi 3B (BCM2837, AArch64, onboard BCM43430A1 Bluetooth)
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

### Build

```bash
# Recommended: Musashi CPU backend with Bluetooth and USB
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 \
  ./scripts/build.sh

# Output: emu68/install-bellatrix-rigel-musashi/
```

**Build options** (environment variables):

| Variable | Default | Description |
|---|---|---|
| `BELLATRIX_CPU_BACKEND` | `emu68` | `emu68` (JIT) or `musashi` (interpreter, faster iteration) |
| `BELLATRIX_BTSTACK` | `0` | Bluetooth HID host (BTStack) |
| `BELLATRIX_USBSTACK` | `0` | USB HID + mass storage (CherryUSB) |
| `BELLATRIX_LAUNCHER` | `1` | ADF/ISO selector UI |
| `BELLATRIX_MULTICORE_BUILD` | `0` | Multi-core mode (Core1=GFX, Core2=Paula, Core3=IO) |
| `BELLATRIX_OSD` | `1` | On-screen display overlay |

### Flash to SD

Copy everything from the install directory to the FAT32 boot partition of the SD card:

```bash
cp emu68/install-bellatrix-rigel-musashi/* /media/user/BOOT/
```

If using Bluetooth or USB, the SD card must also contain these placeholder files (shipped in the install directory):
- `BTPAIRS.TXT` — saved paired device list
- `BTKEYS.TXT` — Bluetooth link keys for automatic reconnection
- `BTSCAN.TXT` — diagnostic log (written on each boot)

---

## Bluetooth pairing

On first boot, the launcher shows a Bluetooth scan screen. Put your keyboard or mouse into pairing mode and press ENTER to pair. The device is saved to `BTPAIRS.TXT` on the SD card.

On subsequent boots, paired devices reconnect automatically — no need to press any pairing button.

---

## Diagnostics

Every chipset register access can be logged over the Pi's serial port (GPIO 14/15, 115200 8N1):

```bash
python3 tools/btrace/btrace.py --port /dev/ttyUSB0 --save boot.jsonl
python3 tools/btrace/analyze.py --unimpl boot.jsonl   # show unimplemented registers
```

Bluetooth diagnostics are written to `BTSCAN.TXT` on the SD card after each scan and connection attempt.

---

## Architecture

Bellatrix is organized as explicit ownership domains, not a collection of isolated chips:

```
BellatrixMachine
 ├── CIA A / CIA B    timers, keyboard, TOD
 ├── Paula            audio, serial, disk, IRQ consolidation (owns INTREQ/INTENA)
 ├── Agnus            beam, DMA arbitration, copper, blitter
 └── Denise           bitplane render, sprites, scanout
```

Key constraints:
- Paula is the only component that derives and publishes IPL to the CPU.
- DMA arbitration lives entirely in Agnus.
- The bus is a synchronization protocol, not just address decode.

See `docs/system_architecture.md` for the full architectural specification.

---

## Repository layout

```
emu68/          Emu68 submodule (upstream, READ-ONLY)
external/       btstack, cherryusb, musashi, rigel, aros (submodules)
src/
  cpu/          bus entry point (bellatrix.h/.c)
  core/         BellatrixMachine, bus trace
  chipset/      CIA, Agnus, Paula, Denise, DMA
  io/           Bluetooth (BTStack HID host), USB (CherryUSB)
  launcher/     Bare-metal ADF/ISO selector UI
  storage/      FAT32, SD card (EMMC), ISO 9660
  host/         Platform Abstraction Layer (PAL), timers
patches/        Minimal diffs applied to emu68/ (6 patches)
tools/harness/  Musashi-based chipset test harness (no hardware needed)
docs/           Architecture specifications
referencias/    READ-ONLY reference copy of Emu68 source
```

---

## Credits

- [Emu68](https://github.com/michalsc/Emu68) by Michal Schulz — M68K→AArch64 JIT engine
- [BTStack](https://github.com/bluekitchen/btstack) — Bluetooth host stack
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) — USB host stack
- [Musashi](https://github.com/kstenerud/musashi) — M68K interpreter (development backend)
- [Rigel](https://github.com/jfdelnero/Rigel) — chipset timing reference
