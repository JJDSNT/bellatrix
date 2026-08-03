# AROS

AROS is the m68k operating system this project boots. It lives at
`external/aros`, a submodule tracking upstream
[aros-development-team/AROS](https://github.com/aros-development-team/AROS),
pinned at **`d0370bd`**.

The submodule is never edited in place. Our work reaches it two ways, and the
split is deliberate.

## Two kinds of change, two mechanisms

| | What | Where it lives | How it gets in |
|---|---|---|---|
| **The port** | 57 files, ~6900 lines — an entire architecture directory that exists nowhere upstream | `aros/arch/m68k-emu68/` in this repository | symlinked into place by `scripts/setup.sh` |
| **Upstream changes** | 19 files modified, 2 added, +186/−11 | `patches/aros/` | applied by `scripts/setup.sh` |

The port is **our source code**, not a modification of anyone else's. Carrying
6900 lines as a patch would produce a diff nobody can review, with no history
of its own, that has to be regenerated on every edit. It lives in this
repository as ordinary source, and `arch/m68k-emu68` inside the AROS tree is a
symlink to it.

That means there is exactly one copy. Editing
`external/aros/arch/m68k-emu68/kernel/…` and editing
`aros/arch/m68k-emu68/kernel/…` are the same operation on the same file — no
copy step, nothing to forget to copy back.

The patch series is reserved for what it is good at: small, reviewable changes
to code that belongs to someone else.

## The series

| # | Patch | What it does |
|---|---|---|
| 0001 | `configure-add-m68k-emu68-target` | Registers an m68k target whose architecture is not `amiga`. Everything else follows from that distinction being expressible. |
| 0002 | `m68k-all-support-non-amiga-m68k` | The shared m68k layer assumed Amiga hardware in cache maintenance, dispatch, signalling, task switch and wait. Adds `preserveall.S` and `preserveall_install.c`. |
| 0003 | `sdcard-initialise-softlist` | **Upstream bug.** `NEWLIST` on `sdcu_SoftList` was lost when the driver was derived from `rom/devs/ata`. `AddHead()` on a zeroed list writes through a NULL `lh_Head`, i.e. to address 4 — harmless-looking on the ARM ports that have used this driver, fatal on m68k where address 4 is `AbsExecBase`. |
| 0004 | `dos-reply-synchronous-system-packet` | **Upstream bug.** A synchronous `System()` had its flags zeroed, so `AROS_CLI()` took the "CliInit already replied for me" branch and nobody ever replied. The caller sits in `WaitPkt()` forever. `__dos_Boot()` hits it on the first `Execute()` that `AROSMonDrvs` makes, which is why no display driver was ever loaded on m68k. |
| 0005 | `dosboot-planar-image-by-arch` | The planar boot image was selected on `AROS_TARGET_CPU=m68k`, which also catches an m68k with a chunky framebuffer and no blitter. Tests `AROS_TARGET_ARCH=amiga` instead. |
| 0006 | `debug-enable-dos-shell-tracing` | **Bring-up aid, not part of the port.** `#define DEBUG 1` in the boot and shell paths. Applied last and touching nothing else, so it can be dropped without disturbing the rest. |

0003 and 0004 are ordinary upstream bugs that this port happened to expose.
Neither mentions m68k-emu68 and both stand alone, which makes them the natural
candidates to send upstream first.

Applying the series and then the symlink reproduces the reference branch
exactly: `git diff` between the two, excluding `arch/m68k-emu68`, is empty.

## Working with it

```bash
./scripts/setup.sh            # apply and link (idempotent)
./scripts/setup.sh --verify   # report state
./scripts/setup.sh --reset    # discard and redo
```

Neither the applied series nor the symlink shows up in `git status`, at either
level — see `patches/README.md` for why, and use `--verify` rather than
`git status` to ask whether the tree is in the expected state.

To edit the series, the workflow is the same as for Emu68 — see
[`emu68.md`](emu68.md), substituting `patches/aros` and the AROS pin. To edit
the port, just edit it.
