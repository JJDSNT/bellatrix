# Bellatrix

Bare-metal Amiga machine emulator for the Raspberry Pi 3B. Integrates the [Emu68](https://github.com/michalsc/Emu68) M68K JIT engine with the [Rigel](https://github.com/jfdelnero/Rigel) Amiga chipset library, running directly on Pi hardware — no Linux, no hypervisor.

The chipset (CIA, Agnus, Paula, Denise) lives in Rigel. Bellatrix owns the machine: bus protocol, runtime, IO (USB HID, Bluetooth HID), SD storage, and the launcher UI.

**Current state:** fully functional with the Musashi M68K interpreter backend. Emu68 JIT integration is in progress — AROS reaches the kitty screen but does not yet complete boot.

---

## Quick start

```bash
git clone --recurse-submodules https://github.com/YOUR_USERNAME/bellatrix
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
- [Rigel](https://github.com/jfdelnero/Rigel) — Amiga chipset library
- [BTStack](https://github.com/bluekitchen/btstack) — Bluetooth host stack
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) — USB host stack
- [Musashi](https://github.com/kstenerud/musashi) — M68K interpreter
