# Bellatrix

Bare-metal Amiga machine emulator for the Raspberry Pi 3B. Integrates the [Emu68](https://github.com/michalsc/Emu68) M68K JIT engine with the [Rigel](https://github.com/JJDSNT/Rigel) Amiga chipset library, running directly on Pi hardware — no Linux, no hypervisor.

Bellatrix owns the machine: bus protocol, multicore runtime, USB HID input (CherryUSB), Bluetooth HID input (BTStack), SD storage, and a bare-metal launcher UI.
The chipset (CIA, Agnus, Paula, Denise) lives in Rigel.

Four kernel images are released as GitHub Release assets: `bellatrix_musashi.img`, `bellatrix_musashi_multicore.img`, `bellatrix_emu68.img`, and `bellatrix_emu68_multicore.img`, each with a `.sha256` checksum. The Musashi builds are the stable ones — boots Kickstart 1.3/Workbench 1.3, AROS, and USB HID are functional on the Pi. Four areas still in progress:

- **Emu68 JIT integration** — bus API and quantum window stabilization ongoing; Musashi builds are the recommended path for now.
- **Multicore** — Core 0 (host), Core 1 (CPU), Core 2 (chipset), Core 3 (IO) split defined but cross-core synchronization not fully resolved.
- **SD card boot (Amiga HD)** — RDB (Rigid Disk Block) support for booting directly from an RDB-partitioned SD card is not yet functional.
- **ISO boot (Amiga CD-ROM)** — booting from ISO images via lide.device is not yet functional; ODFileSystem is the planned filesystem layer.
- **Bluetooth HID** — device scan works; pairing and connection not yet functional.
- **HDMI audio** — not yet implemented; video output only.

---

## Using release images

The released `.img` files are Raspberry Pi kernel images, not complete SD card images. Use one of them on an Emu68-style Raspberry Pi boot FAT32 partition, together with the usual Pi firmware files and a `config.txt`.

Example `config.txt`:

```ini
kernel=bellatrix_musashi.img
initramfs kick.rom
arm_64bit=1
enable_uart=1
```

Copy your Kickstart ROM to the same boot partition and make the `initramfs` line point to that file. For example, with `initramfs kick.rom`, the file must be named `kick.rom` on the SD boot partition. Use a legally obtained Kickstart ROM.

The Musashi images are currently the recommended hardware builds. The Emu68 JIT images are published for testing the JIT integration path.

### AROS ROM

AROS uses a split ROM: an EXT ROM (maps to 0xE00000) and a main ROM (maps to 0xF80000). Bellatrix expects a single 1 MB file produced by concatenating them **in that order**:

```bash
cat aros-ext.bin aros-rom.bin > aros.rom
```

Pass it the same way as a Kickstart ROM (`initramfs aros.rom` in `config.txt`, or via `KICKSTART=` in the harness).

**Boot screen with newer AROS builds**: AROS ROM builds from May 2026 onwards boot correctly when an ADF is inserted, but the boot screen does not appear in the harness without enabling a workaround (`HARNESS_MSGPORT_OWNER_FIX=1`). The Jul/2025 build is the validated baseline. See ISSUE-0020 in `AI_context/` for details.

### Launcher

The Musashi builds include a bare-metal launcher UI that runs before the emulator starts. It reads ADF and ISO files from a FAT32-formatted USB drive and lets you select which one to boot. Insert the USB drive before powering the Pi; the launcher lists available files and waits for input via USB keyboard or HID joystick.

The USB drive must be formatted as FAT32. Place `.adf` files (floppy disk images) or `.iso` files (CD-ROM images) in the root directory or any subdirectory. The launcher also reads from the SD boot partition if no USB drive is present.

---

## Quick start

```bash
git clone --recurse-submodules https://github.com/JJDSNT/bellatrix
cd bellatrix
./scripts/setup.sh   # one-time: apply patches to the emu68 submodule
./run.sh             # build and run (opens a TUI to select ROM and options)
```

### Musashi harness (Linux, no hardware needed)

The primary development loop. Runs the full Rigel chipset with the Musashi interpreter as a native Linux process with an SDL2 window.

```bash
./run.sh harness
```

See `./run.sh --help` for all modes (`qemu`, `harness`, `raspi`, `tftp`) and options.

---

## Repository layout

```
run.sh              entry point — build + run (QEMU / harness / Pi)
scripts/            setup, build, flash helpers
emu68/              Emu68 submodule — M68K JIT (READ-ONLY)
external/           rigel, btstack, cherryusb, musashi, aros (submodules)
src/
  machine/          BellatrixMachine — integration and bus protocol
  runtime/          multicore runtime (CPU / Chipset / IO cores)
  cpu/              bus entry point — bridges Emu68 to Bellatrix
  io/               Bluetooth (BTStack) and USB (CherryUSB) HID
  launcher/         bare-metal ADF/ISO selector UI
  storage/          FAT32, SD card, ISO 9660
  host/             Platform Abstraction Layer
tools/
  harness/          Musashi + Rigel test harness (native Linux)
  launcher/         TUI launcher (Go) for ROM/config selection
docs/               architecture documentation
```

---

## Credits

- [Emu68](https://github.com/michalsc/Emu68) by Michal Schulz — M68K→AArch64 JIT
- [Rigel](https://github.com/JJDSNT/Rigel) — Amiga chipset library
- [BTStack](https://github.com/bluekitchen/btstack) — Bluetooth host stack
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) — USB host stack
- [Musashi](https://github.com/kstenerud/musashi) — M68K interpreter
