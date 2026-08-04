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

## Building

```bash
./scripts/build-aros.sh          # incremental
./scripts/build-aros.sh clean    # wipe, keeping the downloaded tarballs
```

Output: `out/aros/aros-emu68-m68k.elf` — ELF 32-bit MSB, Motorola m68k,
about 1.0 MB.

Two things about this build are not obvious and cost time to rediscover:

**It must be serial.** AROS's mmake does not order the crosstools stage against
generation of the target headers. Under `make -j` the gcc stage can be
configured before `bin/<target>/AROS/Developer/include` is populated, and gcc's
configure then fails with `error verifying int64_t uses long long` — a message
that says nothing about the real cause, roughly fifteen minutes in.

**The metatarget is `kernel-link-emu68-m68k`, not `AROS-emu68-m68k`.** The
latter only *depends* on the former and additionally builds the whole
distribution, contrib and boost included. For the ELF it is pure waste.

The first build also builds an m68k-aros cross toolchain (binutils 2.32 and gcc
6.5.0) from source, which takes far longer than AROS itself. `.installflag-crosstools`
under `out/build/aros/bin/linux-x86_64/tools/crosstools/` is the marker that it
finished — the `m68k-aros-gcc` binary appears well before the toolchain is
actually complete, since libgcc for the target is built in a second phase.

## Running

```bash
./scripts/make-sdcard.sh     # out/aros/sd.img
./run.sh                     # framebuffer in a window, serial on stdout
./run.sh --headless          # serial only
./run.sh --debug InitCode    # sysdebug flags
./run.sh --no-aros           # Emu68 alone
```

QEMU emulates the Raspberry Pi; Emu68 is the bare-metal owner and loads the
m68k ELF from `-initrd`. Four pieces have to line up:

| | Built by |
|---|---|
| `out/images/Emu68.img` | `scripts/build.sh` |
| `out/firmware/bcm2710-rpi-3-b.dtb` | `scripts/build.sh` (Emu68's cmake downloads it) |
| `out/aros/aros-emu68-m68k.elf` | `scripts/build-aros.sh` |
| `out/aros/sd.img` | `scripts/make-sdcard.sh` |

**The card needs the full distribution tree**, not just the ELF — `C`, `S`,
`Libs`, `Devs`, `L`, `Classes`, `Fonts`, `System`, `Prefs`, `Storage`,
`Utilities`, `Tools`, `Locale` and `AROS.boot`. The lean `kernel-link` target
does not produce them, so `make-sdcard.sh` takes `--dist DIR` to point at a
tree that has them.

Three constraints are encoded in the scripts rather than left to be
rediscovered:

- **`Locale` is not optional.** `S:Startup-Sequence` does
  `Assign "LOCALE:" "SYS:Locale"`; without it the console opens with
  `Can't find SYS:Locale` and every later `LOCALE:`-relative assign is built
  on sand.
- **Do not copy `Developer`.** It is ~291 MB of SDK that nothing in the boot
  path reads, and a card carrying it stalls the boot between `AROSMonDrvs` and
  "preparing console".
- **`nocomposition` is currently required** to see anything on the
  framebuffer. Without it the boot completes and the screen stays on the Emu68
  logo — `emu68gfx` is presumably missing something the software compositor
  expects of a driver it has taken over.

`sysdebug=` is parsed out of the kernel arguments into `SysBase->ex_DebugFlags`,
so AROS's runtime debug flags work with no rebuild. `InitCode` is the most
useful during bring-up; the full list is `ExecFlagNames` in
`rom/exec/exec_flags.c`.

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
