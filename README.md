# Bellatrix

Bare-metal Amiga machine emulator for the Raspberry Pi 3B. Integrates the [Emu68](https://github.com/michalsc/Emu68) M68K JIT engine with the [Rigel](https://github.com/jfdelnero/Rigel) Amiga chipset library to produce a complete Amiga-compatible machine running directly on Pi hardware — no Linux, no hypervisor.

---

## Running

`./run.sh` is the single entry point. Run it without arguments for a TUI that lets you select ROM and options.

```bash
git clone --recurse-submodules https://github.com/YOUR_USERNAME/bellatrix
cd bellatrix
./scripts/setup.sh   # one-time patch setup
./run.sh             # build and run
```

See `./run.sh --help` for all modes and options.

---

## Credits

- [Emu68](https://github.com/michalsc/Emu68) by Michal Schulz — M68K→AArch64 JIT
- [Rigel](https://github.com/jfdelnero/Rigel) — Amiga chipset library (CIA, Agnus, Paula, Denise)
- [BTStack](https://github.com/bluekitchen/btstack) — Bluetooth host stack
- [CherryUSB](https://github.com/cherry-embedded/CherryUSB) — USB host stack
- [Musashi](https://github.com/kstenerud/musashi) — M68K interpreter
