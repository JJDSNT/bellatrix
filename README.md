# Bellatrix
> **An AI-generated project, inspired by Bloodline's [Omega](https://github.com/h5n1xp/Omega).**

A bare-metal Amiga machine emulator for the Raspberry Pi 3B. Integrates the [Emu68](https://github.com/michalsc/Emu68) M68K JIT engine with the [Rigel](https://github.com/JJDSNT/Rigel) Amiga chipset library, running directly on Pi hardware — no Linux, no hypervisor.

Bellatrix owns the machine: bus protocol, multicore runtime, USB HID input (CherryUSB), Bluetooth HID input (BTStack), SD storage, and a bare-metal launcher UI.
The chipset (CIA, Agnus, Paula, Denise) lives in Rigel.

Six kernel variants are released as GitHub Release assets: Musashi 68000,
Musashi 68040, and Emu68, each in single-core and multicore form. Their names
are `bellatrix_musashi_68000.img`, `bellatrix_musashi_68040.img`,
`bellatrix_emu68.img`, plus the corresponding `_multicore.img` files. Every
image includes a `.sha256` checksum. The Musashi builds are the stable ones —
Kickstart 1.3/Workbench 1.3, AROS, USB HID, and Bluetooth HID are functional
on the Pi. Five areas remain in progress:

- **Emu68 JIT integration** — the Bellatrix adapter reaches the AROS boot
  screen and boots Kickstart with its bus dispatched through Emu68's native
  fault-driven external-bus path (Data Abort as MMIO router, not a limitation
  to remove — see [`docs/fault_handler.md`](docs/fault_handler.md)). Chipset
  time advances every `MainLoop` pass independently of that routing, so
  RAM-only code paths (idle loops) don't stall the emulated clock — see
  [`docs/emu68_internals.md`](docs/emu68_internals.md). Musashi 68040 remains
  the recommended path for stability; Emu68 is where active integration work
  happens.
- **SD card boot (Amiga HD)** — RDB (Rigid Disk Block) support for booting directly from an RDB-partitioned SD card is not yet functional.
- **ISO boot (Amiga CD-ROM)** — booting from ISO images via lide.device is not yet functional; ODFileSystem is the planned filesystem layer.
- **RTG support** — `bellatrix.rtg` and the P96 `bellatrix.card` path are in progress; DiagArea/CardLoader residency is confirmed, but p96gfx discovery, live RTG output, and the VC4 bare-metal backend still need validation/completion.
- **HDMI audio** — the bare-metal HDMI output path is believed to be working:
  the DMA test clip/WAV plays on real Pi hardware. The remaining work is to
  get Amiga/Paula emulation fast enough on hardware to validate real Amiga
  audio through that path.

Bluetooth HID (BTStack host on the Pi 3B's onboard BCM43430A1) is functional:
device scan, pairing, and keyboard/mouse/joystick input all work end-to-end.
Refinements to pairing robustness and reconnection are ongoing.

The multicore runtime is functional and, as of the 2026-07-15 topology
rebaseline, identical for both CPU backends: selecting Emu68 or Musashi only
swaps which implementation runs on the CPU core. The **target** architecture
keeps Core 0 as the Host Reactor (supervision plus physical I/O); the
**current, temporary** stabilization placement instead runs the CPU on
Core 0 (to minimize Emu68 integration variables), the Host Reactor on Core 3,
and leaves Core 1 auxiliary/parked, while Core 2 exclusively owns and
advances Rigel either way. See [`docs/runtime_and_timing.md`](docs/runtime_and_timing.md)
and [`docs/host_reactor.md`](docs/host_reactor.md) for the full mapping and
why it's temporary. Launcher and runtime share one USB/CherryUSB owner and
service path. CPU/chipset backpressure, critical-MMIO
rendezvous, atomic IPL publication, deadline scheduling, and non-blocking
cross-core serial queues are implemented. Remaining multicore work is hardware
validation under combined I/O load and performance tuning rather than a missing
runtime architecture component. The Emu68/JIT multicore path remains
experimental together with the wider Emu68 integration.

The Host Reactor currently polls at approximately 1 kHz. On a Raspberry Pi 3B
its measured runtime cost is about 7 us average and 26 us maximum per dispatch,
with no missed 1 ms budgets in the validated workload. See
[`docs/host_reactor.md`](docs/host_reactor.md) for ownership, IRQ direction,
single-core behavior and current synchronous-MSC limitations.

---

## Using release images

The released `.img` files are Raspberry Pi kernel images, not complete SD card images. Use one of them on an Emu68-style Raspberry Pi boot FAT32 partition, together with the usual Pi firmware files and a `config.txt`.

Example `config.txt`:

```ini
kernel=bellatrix_musashi_68040.img
initramfs kick.rom
arm_64bit=1
enable_uart=1
```

Copy your Kickstart ROM to the same boot partition and make the `initramfs` line point to that file. For example, with `initramfs kick.rom`, the file must be named `kick.rom` on the SD boot partition. Use a legally obtained Kickstart ROM.

The Musashi 68040 images are currently the recommended hardware builds. The
Emu68 JIT images are published for testing the JIT integration path; AROS has
been validated through its boot screen, but the backend remains experimental.

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

### Keyboard shortcuts

These work both in the launcher and while the emulated machine is running:

| Key | Action |
| --- | --- |
| **F11** | Open the Bluetooth scan screen |
| **F12** | Open the media (ADF/ISO) selection screen |

---

## Quick start

### Prerequisites (Ubuntu/Debian)

```bash
sudo apt-get install -y cmake gcc-aarch64-linux-gnu g++-aarch64-linux-gnu golang-go libsdl2-dev
```

`cmake` and the `aarch64-linux-gnu` cross-compiler are needed to build the Pi image and the Musashi harness; `golang-go` builds the `tools/launcher` ROM/config picker TUI (used by `./run.sh` and `./run.sh harness` whenever `KICKSTART` isn't set). `libsdl2-dev` is recommended for the harness — without it, CMake silently falls back to a headless build (no display window).

You also need a Docker-compatible container runtime on your `PATH` as `docker` (Docker Engine, Docker Desktop, Podman with a `docker` alias, or any other OCI-compatible CLI that understands `docker run`-style flags). `./run.sh harness` uses it to cross-compile `lide.device` and `ODFileSystem` for m68k-AmigaOS (image `amigadev/crosstools:m68k-amigaos`) and embed the resulting ROMs into the harness binary.

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
  runtime/          Host Reactor and CPU/chipset multicore runtime
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
