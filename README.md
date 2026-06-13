# Bellatrix

Bellatrix is a bare-metal Amiga machine emulator for the Raspberry Pi 3B. It integrates the [Emu68](https://github.com/michalsc/Emu68) M68K JIT engine with the [Rigel](https://github.com/jfdelnero/Rigel) Amiga chipset library and a full suite of bare-metal IO subsystems (USB HID, Bluetooth HID, SD card, framebuffer), producing a complete Amiga-compatible machine that runs directly on Pi hardware — no Linux, no hypervisor.

---

## Quick start

```bash
git clone --recurse-submodules https://github.com/YOUR_USERNAME/bellatrix
cd bellatrix
./scripts/setup.sh   # one-time: apply patches to emu68/ submodule
./run.sh             # build and run in QEMU (opens a TUI to select ROM/config)
```

`./run.sh` is the single entry point for every workflow — QEMU emulation, the Musashi harness, and flashing to SD card.

---

## Architecture

Bellatrix is the machine layer that sits between the CPU backend and the chipset:

```
┌────────────────────────────────────────────────┐
│              Amiga software                     │
│         (Kickstart / AROS / demos)             │
└────────────────────┬───────────────────────────┘
                     │  M68K instructions
              ┌──────┴──────┐
              │    Emu68    │  ← CPU backend (default)
              │  M68K JIT   │    or Musashi (interpreter)
              └──────┬──────┘
                     │  bus accesses
┌────────────────────▼───────────────────────────┐
│                 Bellatrix                       │
│  machine runtime · bus protocol · IO · timing  │
│                     │                          │
│         ┌───────────▼──────────┐               │
│         │        Rigel         │               │
│         │   Amiga chipset      │               │
│         │  CIA·Agnus·Paula     │               │
│         │  Denise              │               │
│         └──────────────────────┘               │
└────────────────────────────────────────────────┘
             Raspberry Pi 3B bare metal
```

**Emu68** translates M68K to AArch64 JIT. All chipset/CIA bus accesses trigger a data abort that Emu68 routes to Bellatrix.

**Rigel** is the Amiga chipset library (extracted from Bellatrix into its own project). It owns CIA, Agnus, Paula, and Denise — timing, DMA arbitration, interrupt consolidation.

**Bellatrix** owns the machine integration: bus protocol, chip RAM MMU mapping, multicore runtime, launcher UI, Bluetooth and USB HID input, and SD card storage.

---

## Running

### QEMU (development)

```bash
./run.sh                                          # TUI selects ROM and options
./run.sh qemu                                     # same, explicit mode
KICKSTART=src/roms/aros.rom ./run.sh qemu         # skip TUI, run AROS
KICKSTART=src/roms/KS31.rom ./run.sh qemu         # run Kickstart 3.1
EMU_PROFILE=bellatrix-musashi ./run.sh qemu       # use Musashi instead of Emu68
```

### Musashi harness (Linux, no hardware)

The harness runs the full Rigel chipset with a Musashi M68K interpreter as a native Linux process. It is the primary development loop — no Pi, no QEMU, no cross-compile required. It opens an SDL2 window by default; pass `FRAMES` or `CYCLES` for headless runs.

```bash
./run.sh harness                                   # TUI selects ROM
KICKSTART=src/roms/aros.rom ./run.sh harness       # interactive SDL2 window
KICKSTART=src/roms/KS13.rom ADF=disks/WB13.adf ./run.sh harness
KICKSTART=src/roms/aros.rom FRAMES=50 ./run.sh harness    # headless, stop after 50 frames
KICKSTART=src/roms/KS31.rom CYCLES=5000000 ./run.sh harness
```

**CPU model:**

```bash
HARNESS_CPU=68000    # default
HARNESS_CPU=68020    # also: 68010, 68ec020
```

**Serial injection** — automates boot sequences that require keyboard input on the serial port (e.g. AROS boot menu, DiagROM commands). Bytes are injected into Paula's UART RX at specific frame numbers:

```bash
# Inject a single early byte (e.g. space to dismiss a boot menu at frame 5)
HARNESS_SERIAL_BOOTKEY=' ' KICKSTART=src/roms/aros.rom ./run.sh harness

# Inject two bytes at specific frames
HARNESS_SERIAL_BOOTKEY=' ' HARNESS_SERIAL_BOOTKEY_FRAME=900 \
  HARNESS_SERIAL_INJECT=4 HARNESS_SERIAL_INJECT_FRAME=1050 \
  KICKSTART=src/roms/DiagROM.rom ./run.sh harness

# Script: comma-separated frame:byte pairs
HARNESS_SERIAL_SCRIPT="900:0x20,1050:4" KICKSTART=src/roms/DiagROM.rom ./run.sh harness

# Hold: repeat a byte every N frames for a count
HARNESS_SERIAL_HOLD=0x20 HARNESS_SERIAL_HOLD_FRAME=900 \
  HARNESS_SERIAL_HOLD_COUNT=40 HARNESS_SERIAL_HOLD_STEP=1 \
  HARNESS_SERIAL_INJECT=4 HARNESS_SERIAL_INJECT_FRAME=1050 \
  KICKSTART=src/roms/DiagROM.rom ./run.sh harness
```

**Mouse simulation** — holds the right mouse button for N frames (useful for triggering Early Startup menus):

```bash
HARNESS_MOUSE_RMB_FRAME=900 HARNESS_MOUSE_RMB_COUNT=60 \
  HARNESS_SERIAL_INJECT=4 HARNESS_SERIAL_INJECT_FRAME=1050 \
  KICKSTART=src/roms/DiagROM.rom ./run.sh harness

# Inject a serial byte after a real SDL right-click
HARNESS_SERIAL_AFTER_RMB=4 HARNESS_SERIAL_AFTER_RMB_DELAY=80 \
  KICKSTART=src/roms/DiagROM.rom ./run.sh harness
```

**Serial PTY mode** — exposes Paula serial as a PTY; attach with `screen`:

```bash
KICKSTART=src/roms/DiagROM.rom ./run.sh harness-serial
# → prints: screen /dev/pts/X 9600
# → press Enter here after attaching to continue boot
```

### Raspberry Pi 3B

```bash
./run.sh raspi /media/user/BOOT    # build and flash to SD card
./run.sh tftp                      # build and upload via TFTP
```

---

## Runtime profiles

`EMU_PROFILE` selects the CPU backend and install directory:

| Profile | CPU | Install directory |
|---|---|---|
| `bellatrix` _(default)_ | Emu68 JIT | `emu68/install-bellatrix-rigel/` |
| `bellatrix-musashi` | Musashi interpreter | `emu68/install-bellatrix-rigel-musashi/` |
| `emu68` | Emu68 upstream (no Bellatrix) | `emu68/build/` |

Each profile has two core modes:

| | Single-core | Multi-core (`BELLATRIX_MULTICORE_BUILD=1`) |
|---|---|---|
| All on Core 0 | ✓ | — |
| Core 0 = CPU, Core 1 = Chipset (Rigel), Core 3 = IO | — | ✓ |

---

## Key options

```bash
KICKSTART=<path>                  # ROM to boot (required for most modes)
ADF=<path>                        # mount ADF image as DF0
ISO=<path>                        # mount ISO as CD-ROM
EMU_PROFILE=bellatrix-musashi     # switch CPU backend
BELLATRIX_MULTICORE_BUILD=1       # enable multicore runtime
BELLATRIX_BTSTACK=1               # enable Bluetooth HID host
BELLATRIX_USBSTACK=1              # enable USB HID + mass storage
DISPLAY_MODE=none                 # headless QEMU (no window)
FRAMES=<n>                        # harness: stop after N frames (headless)
CYCLES=<n>                        # harness: stop after N M68K cycles
```

Full option reference: `./run.sh --help`

---

## Hardware requirements

- Raspberry Pi 3B (BCM2837 AArch64, onboard BCM43430A1 Bluetooth)
- MicroSD card — FAT32 boot partition
- Kickstart or AROS ROM (not included)
- Optional: USB keyboard/mouse, Bluetooth HID devices, USB drive

---

## Bluetooth pairing (bare metal)

On first boot the launcher shows a Bluetooth scan screen. Put the device into pairing mode and press ENTER. The device address and link key are saved to `BTPAIRS.TXT` and `BTKEYS.TXT` on the SD card. On subsequent boots, paired devices reconnect automatically.

The SD card must contain pre-sized placeholder files for these (shipped in the install directory — the FAT32 writer can only overwrite in place, never allocate new clusters).

---

## Diagnostics

```bash
# Bus trace over serial (GPIO 14/15, 115200 8N1)
python3 tools/btrace/btrace.py --port /dev/ttyUSB0 --save boot.jsonl
python3 tools/btrace/analyze.py --unimpl boot.jsonl

# Bluetooth diagnostic log
# → BTSCAN.TXT on SD card, written after each scan + connection attempt
```

---

## Repository layout

```
run.sh              single entry point — build + run (QEMU / harness / raspi / tftp)
scripts/
  setup.sh          one-time: apply patches to emu68/ submodule
  build.sh          cmake + make (called by run.sh)
  flash.sh          copy to SD or TFTP upload
emu68/              Emu68 submodule — M68K JIT (READ-ONLY upstream)
external/           rigel, btstack, cherryusb, musashi, aros (submodules)
src/
  cpu/              bus entry point — Emu68 data abort → Bellatrix
  core/             BellatrixMachine, bus protocol, bus trace
  runtime/          multicore runtime (core_cpu, core_chipset, core_io)
  io/
    bluetooth/      BTStack HID host
    usb/            CherryUSB HID + MSC
    hid/            shared HID→Amiga rawkey table
  launcher/         bare-metal ADF/ISO selector UI (VC4 framebuffer)
  storage/          FAT32, EMMC/SD, ISO 9660
  host/             Platform Abstraction Layer (IPL, timer, debug)
tools/
  harness/          Musashi + Rigel test harness — runs on Linux, no hardware
  launcher/         TUI launcher (Go) — ROM/config selector for QEMU and harness
patches/            minimal diffs applied to emu68/ (6 patches)
docs/               architecture specifications
```

---

## Current focus

- **Emu68 integration** — full JIT path running with Rigel chipset
- **AROS boot** — reaches the kitty screen; boot sequence does not yet complete

---

## Credits

- [Emu68](https://github.com/michalsc/Emu68) by Michal Schulz — M68K→AArch64 JIT
- [Rigel](https://github.com/jfdelnero/Rigel) — Amiga chipset library (CIA, Agnus, Paula, Denise)
- [BTStack](https://github.com/bluekitchen/btstack) — Bluetooth host stack
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) — USB host stack
- [Musashi](https://github.com/kstenerud/musashi) — M68K interpreter
