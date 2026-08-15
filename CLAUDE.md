# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`AGENTS.md` carries the coding-style, commit and PR conventions. This file covers
the architecture and the traps that only show up after reading several files.

## What this is

AROS/m68k on a Raspberry Pi 3 under Emu68, with no Amiga chipset. Emu68 owns the
bare metal and JITs M68K to AArch64; AROS starts after Emu68 has initialised the
hardware and loaded its m68k ELF from `-initrd`. Both are upstream projects,
vendored as pinned submodules and never edited in place.

## Commands

```bash
./scripts/setup.sh           # init submodules, apply patch series, install symlinks
./scripts/setup.sh --verify  # report state; use this, not `git status` (see below)
./scripts/setup.sh --reset   # discard submodule working-tree changes and re-apply

./scripts/build.sh [clean]       # Emu68     → out/images/Emu68.img, out/firmware/
./scripts/build-aros.sh          # AROS m68k → out/aros/aros-emu68-m68k.elf
./scripts/build-aros.sh --status # what would a build cost? builds nothing
./scripts/make-sdcard.sh         # boot media → out/aros/sd.img

./run.sh                     # boot under QEMU: AROS if its ELF exists, else Emu68 alone
./run.sh --headless          # serial only, no window
./run.sh --debug FLAGS       # appends sysdebug=FLAGS to the kernel arguments
./run.sh -- <qemu args>      # everything after -- goes to qemu
```

Everything generated lands under `out/`, which is git-ignored.

- **`build-aros.sh` is deliberately serial.** AROS's mmake does not order the
  crosstools stage against generation of the target headers; under `make -j` gcc
  configures against a half-built sysroot and dies ~15 minutes in with
  `error verifying int64_t uses long long`. Do not "fix" this with `-j`.
- **The m68k cross toolchain is the expensive thing, and it is not the build.**
  The first `build-aros.sh` compiles binutils and gcc from source — hours, and
  ~650 MB under `out/build/aros/bin/<host>/tools/crosstools`. Everything else
  here is minutes by comparison, so:
  - **`build-aros.sh clean` keeps the toolchain**; `distclean` is the verb that
    drops it. Reach for `distclean` only when the toolchain itself is suspect.
  - A build that would have to compile the toolchain **refuses** when there is
    no terminal to ask, which is the case for any agent driving the shell. Pass
    `--yes` (or `BELLATRIX_BUILD_YES=1`) to accept the cost deliberately.
  - **`build-aros.sh --status` answers "what would this rebuild?"** — toolchain
    state, whether the tree is configured, whether the submodules verify,
    whether the distribution tree exists. Ask it before reaching for `clean`.
  - The toolchain is stamped with a digest of what it was built from
    (`config/gcc_def`, `config/binutils_def`, `tools/crosstools`), so a `clean`
    can tell whether keeping it is sound. That digest deliberately ignores our
    own sources and the rest of the patch series: they change daily and the
    toolchain does not depend on them.
- `build-aros.sh` builds `kernel-link-<target>` by default: the ELF and the
  modules linked into it. **`build-aros.sh full` builds `AROS-<target>`**, the
  whole distribution — slower, drags in contrib and fetches external sources,
  and it is what the SD card should be made from.
- **The lean build is not enough to test a change to module code.** Libraries,
  Zune classes and the commands in `C:` are separate files on the card, taken
  from whatever tree `make-sdcard.sh --dist` points at. Until 2026-08-07 that
  was always a foreign reference tree, so patches touching those never reached
  what booted — silently. If a change is not in the kernel ELF, check where the
  module on the card came from.
- `run.sh` opens a QEMU monitor on `/tmp/emu68-monitor.sock`; `nc -U` it and use
  `screendump` to capture the framebuffer headlessly.

## How upstream gets modified

Two mechanisms, chosen by what the change *is*:

**Patches** — for changing code that belongs to someone else. `patches/<name>/`
maps to `external/<name>/`, numbered from `0001` per series. `setup.sh` discovers
series from the directory layout (adding one means adding a directory), applies
them in numeric order because a later patch may edit a region an earlier one
reshaped, and verifies the result by a **tree hash derived from the patches
themselves** — there is no expected hash recorded anywhere to drift.

**Symlinks** — for shipping our own code into a submodule tree. A top-level
directory named after a submodule mirrors that submodule: `aros/arch/m68k-emu68/`
is linked to `external/aros/arch/m68k-emu68`. The injection point is the first
level that does *not* already exist upstream, so nothing has to be declared.
There is exactly one copy of these files; editing them from either path is the
same file.

### The trap

**An applied series does not appear in `git status`, at either level.** The
parent ignores submodule working-tree changes (`ignore = dirty` in
`.gitmodules`), and `setup.sh` marks patched files `skip-worktree` inside the
submodule. Both are deliberate — the applied series is the normal working state.

That also hides *genuine* local edits inside a submodule. Always ask
`./scripts/setup.sh --verify`, which reads the working tree through a scratch
index and reports `pristine`, `applied`, `dirty` or `broken:<patch>`. A `dirty`
result means someone edited the submodule directly and the edit exists in no
patch — capture it before running `--reset`, which will destroy it silently.

To find *what* drifted: check out the pinned commit in a scratch worktree, apply
the series into it, and diff against `external/<name>/` file by file.

## Boot chain and the IRQ bridge

QEMU `raspi3b` → `-kernel Emu68.img` + `-dtb` → `-initrd aros-*.elf` →
`-drive if=sd` mounts as `SDCARD0P0:` → `S:Startup-Sequence` → Shell → Wanderer.

Physical BCM283x interrupts do not take the Amiga Paula path. The chain is:

```
ARM peripheral → Emu68 ARMPending → m68k level-6/EXTER → Platform_Autovector()
→ ARM interrupt-controller Dispatch() → krnRunIRQHandlers() → driver handler
```

Emu68 raises level 6 only when the guest `INTENA` shadow has both `INTEN` and
`EXTER`; `Platform_Init()` arms that gate once, and pending state clears only
when the guest acknowledges EXTER through `INTREQ`. `KrnCli()`/`KrnSti()` are
*not* the physical IRQ gate on this port. `docs/irq.md` documents the three
possible delivery mechanisms (shadow registers, MOVEC, PiStorm's IRQ line) and
which one is still an open design question.

The `nocomposition` boot argument is currently required to see anything on the
framebuffer.

## Repository conventions

- **Everything in the repository is written in English** — docs, issues, specs,
  code comments, commit messages. Conversation with the user is in Portuguese.
- `docs/` describes what is true *now*; `AI_context/` describes work in progress.
  A doc whose premise stops holding is not deleted — it gets a correction header
  and stays while any part of its design is still relevant.
- `AI_context/issues/` are living documents with YAML frontmatter
  (`status`, `priority`, `type`, `blockers`); closed ones move to
  `AI_context/consolidated/history/`. `AI_context/README.md` has the grep
  recipes for querying state — no tooling needed.

## Measuring a boot

The AROS boot to desktop is **timing-sensitive and intermittent**. Never conclude
from a single run: use at least 3 serial runs per configuration, alternating
configurations, on an idle machine (`ps -eo cmd | grep -c '[q]emu-system'` must
be 0 first). Concurrent QEMU instances change the outcome. Capture objectively
with `screendump` and compare dominant colour — the Emu68 logo screen and the
Workbench screen are both grey.
