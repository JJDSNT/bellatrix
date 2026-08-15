# Bellatrix

> An AI-generated project, inspired by Bloodline's
> [Omega](https://github.com/h5n1xp/Omega).
>
> The goal is AROS running at a usable speed on a Raspberry Pi 3 — which is
> where it is built and tested, not a limit of the idea. Nothing in the
> approach is specific to this board, and a stronger one, a Radxa Orion O6 say,
> would have more room than the Pi ever will.

AROS/m68k on a Raspberry Pi 3, with no Amiga anywhere in it.

Nothing here emulates a machine. There is no chipset, no Paula, Agnus or
Denise, no simulated device and no timing model. M68K is treated as one more
instruction set the board can run: Emu68 translates it to AArch64 as it
executes, and the code works against real RAM and the Pi's own peripherals —
closer to what Rosetta does for x86 on Apple silicon than to what UAE does for
an Amiga.

Classic chipset compatibility is wanted later, through
[Rigel](https://github.com/JJDSNT/Rigel), for the software that needs Paula and
friends to exist. It would sit beside this path rather than under it: native Pi
hardware reached through m68k drivers, classic hardware semantics through
Rigel, kept as separate domains.

Emu68 owns the bare metal and hands control to AROS once the hardware is up.
Both are upstream projects, vendored as submodules and never edited in place.

## Quick start

On Debian or Ubuntu, install what the build needs:

```bash
sudo apt-get install -y gcc-aarch64-linux-gnu cmake flex bison gperf \
    python3 mtools qemu-system-arm
```

Then:

```bash
git clone --recurse-submodules git@github.com:JJDSNT/bellatrix.git
cd bellatrix

./scripts/setup.sh          # check out the submodules, apply the patch series
./scripts/build.sh          # Emu68        → out/images/Emu68.img
./scripts/build-aros.sh     # AROS m68k    → out/aros/aros-emu68-m68k.elf
./scripts/make-sdcard.sh    # boot media   → out/aros/sd.img
./run.sh                    # boot the lot under QEMU
```

Set aside a few hours for the first `./scripts/build-aros.sh`: it builds an
m68k cross toolchain from source before it can build AROS itself, and that
takes longer than everything else combined. Later runs reuse it.

`run.sh` boots whatever is built: AROS if its ELF is there, otherwise Emu68 on
its own. `--no-aros` forces the latter, `--headless` drops the window.

## Running on a Raspberry Pi 3

You need a Pi 3 (Model B or B+), a microSD card of 2 GB or more, an HDMI
monitor and a wired USB mouse. Everything else is built here.

### 1. Build

```bash
./scripts/build.sh              # Emu68
./scripts/build-aros.sh full    # AROS, complete — the plain build is not enough
```

### 2. Prepare the card

The card uses the Emu68 layout: MBR, and the first partition formatted FAT32.
Nothing else is required of it, and its size is yours to choose — use the whole
card if you want to.

### 3. Pack and unpack

```bash
./scripts/make-sdcard.sh --pack     # → out/aros/bellatrix-pi3.tar.xz
```

Unpack it at the root of that partition, with `/media/you/BOOT` replaced by
wherever the card is mounted:

```bash
tar -xJf out/aros/bellatrix-pi3.tar.xz -C /media/you/BOOT
```

That is everything the Pi reads at power-on — firmware, Emu68, AROS and the
settings — alongside the AROS system files.

### 4. Boot it

Insert the card, connect the monitor and the mouse, and power up. The loading
screen appears first, then the Workbench desktop.

### If it does not come up

**The boot stops on the loading screen.** Some USB devices stop it there.
Power off, unplug everything from USB, and boot again; add the mouse back once
you know the machine comes up. A plain wired mouse is the safe choice.

**Nothing at all on the monitor.** The green LED next to the card slot says how
far the Pi itself got. If it never blinks, the card was not read at all — check
that the first partition is FAT32, and try another card or another reader. Four
or seven blinks mean files are missing from it: unpack the archive again, at
the root of that partition.

**A black screen with the LED behaving normally.** Turn the monitor on before
powering the Pi. If it stays black, add the line `hdmi_force_hotplug=1` to
`config.txt` on the card — that makes the Pi drive the output even when it
cannot detect the monitor at power-on.

### What works today

The Pi boots to the Workbench desktop. USB input is still being brought up, so
some devices are not usable yet.

## Layout

```
external/emu68      submodule → michalsc/Emu68            (pinned 9b4379a)
external/aros       submodule → aros-development-team/AROS (pinned d0370bd)

aros/arch/m68k-emu68  the AROS port — our source, symlinked into the AROS tree
patches/emu68/        3 patches on Emu68
patches/aros/         6 patches on AROS

scripts/            setup, build, build-aros, make-sdcard
run.sh              boot under QEMU (Emu68, or Emu68 + AROS + SD card)
out/                everything generated (git-ignored)

docs/               reference documentation
AI_context/         issues and consolidated knowledge
```

## How upstream is modified

Two mechanisms, chosen by what the change *is*:

**Patches**, for changing code that belongs to someone else. Both series are
small and cut by purpose — 3 patches on Emu68 (+206/−14 across 4 files), 6 on
AROS (+220/−9 across 21 files). `scripts/setup.sh` applies them, and checks the
result by a tree hash derived from the patches themselves.

**Symlinks**, for shipping our own. The port under `aros/arch/m68k-emu68` is
57 files and ~6900 lines; as a patch it would be an unreviewable diff with no
history of its own. It lives here as ordinary source and is linked into
`external/aros/arch/m68k-emu68`, so there is exactly one copy and editing it
from either path is the same file.

An applied series does not appear in `git status`, at either level — that is
deliberate, since it is the normal working state. **Use
`./scripts/setup.sh --verify` rather than `git status`** to ask whether a
submodule is as expected; it reads the working tree through a scratch index and
reports `pristine`, `applied`, `dirty` or `broken`.

## Documentation

| | |
|---|---|
| [`docs/Compat.md`](docs/Compat.md) | the compatibility objective: AROS as the resident system, with AROS and both families of AmigaOS userland booting on it, no Kickstart ROM required |
| [`docs/emu68.md`](docs/emu68.md) | what the Emu68 patches change, where, and which patch each change comes from |
| [`docs/aros.md`](docs/aros.md) | the same for AROS, plus building and running |
| [`docs/irq.md`](docs/irq.md) | how a host interrupt becomes an m68k interrupt, and the three mechanisms available for it |
| [`patches/README.md`](patches/README.md) | the patch and injection conventions |
| [`AI_context/`](AI_context/) | open issues and consolidated knowledge |

Nothing in either patch series is specific to this project — all of it is a
candidate for upstreaming. Two of the nine are ordinary upstream bugs that this
work happened to expose, and neither mentions this port:
`sdcard` missing a `NEWLIST` before `AddHead()` writes through a NULL `lh_Head`
(address 4, which on m68k is `AbsExecBase`), and a synchronous `System()` that
never replies its startup packet, leaving the caller in `WaitPkt()` forever.

## Status

Working, on a Raspberry Pi 3 Model B and under QEMU alike:

- the machine boots to the Workbench desktop;
- the SD card is the system volume — AROS runs from the same card it booted
  from;
- the display comes up on HDMI, and a wired USB mouse drives the desktop.

Still being brought up:

- **USB input.** Some devices stop the boot before the desktop appears; the
  machine has to be started without them, and a plain wired mouse is the safe
  choice. Which devices, and why, is what is being worked on now.
- **Storage.** The card is read correctly, but the driver stumbles once during
  start-up on real hardware and recovers by resetting the controller.
- **The display path.** `nocomposition` is currently required; with the
  compositor enabled the boot finishes but the screen never changes.

Eight issues are filed under [`AI_context/issues/`](AI_context/issues/).

## Language

All content in this repository — documentation, issues, specs, code comments
and commit messages — is written in English.

## Acknowledgement

Special thanks to Claude for the guidance and support throughout the
development of Bellatrix.

## Credits

Bellatrix is the integration of two projects it does not own, and it would not
exist without either:

- [Emu68](https://github.com/michalsc/Emu68) by Michal Schulz — the M68K→AArch64
  translator that makes m68k code run on the board at all;
- [AROS](https://github.com/aros-development-team/AROS) — the operating system,
  and the m68k port this one descends from.

Both are vendored as submodules and used unmodified except for the patch series
under `patches/`, all of which is written to be upstreamable.
