# Bellatrix

AROS/m68k running on a Raspberry Pi 3 under Emu68, with no Amiga hardware
anywhere — an m68k CPU with real RAM, and Pi peripherals reached directly
rather than through Paula, Agnus and Denise.

Emu68 owns the bare-metal machine and translates M68K to AArch64. AROS starts
after Emu68 has initialised the hardware and loaded its ELF. Both are upstream
projects, vendored as submodules and never edited in place.

## Quick start

```bash
git clone --recurse-submodules git@github.com:JJDSNT/bellatrix.git
cd bellatrix

./scripts/setup.sh          # check out the submodules, apply the patch series
./scripts/build.sh          # Emu68        → out/images/Emu68.img
./scripts/build-aros.sh     # AROS m68k    → out/aros/aros-emu68-m68k.elf
./scripts/make-sdcard.sh    # boot media   → out/aros/sd.img
./run.sh                    # boot the lot under QEMU
```

The first `build-aros.sh` also builds an m68k cross toolchain from source and
takes considerably longer than everything else combined.

`run.sh` boots whatever is built: AROS if its ELF is there, otherwise Emu68 on
its own. `--no-aros` forces the latter, `--headless` drops the window.

Prerequisites: `gcc-aarch64-linux-gnu`, `cmake`, `flex`, `bison`, `gperf`,
`python3`, `mtools`, `qemu-system-arm`.

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

Verified here: Emu68 boots under QEMU; the AROS m68k ELF builds and is loaded;
the SD card mounts as `SDCARD0P0:`; `S:Startup-Sequence` runs and the Shell
executes. The desktop has been reached in earlier runs of this port but is not
part of what this repository has verified yet.

Five open issues are filed under `AI_context/issues/` — four defects in
upstream Emu68 found while building this, and one in our own patch series.

## Language

All content in this repository — documentation, issues, specs, code comments
and commit messages — is written in English.
