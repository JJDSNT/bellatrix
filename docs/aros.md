# AROS

AROS is the m68k operating system this project boots. It lives at
`external/aros`, a submodule tracking upstream
[aros-development-team/AROS](https://github.com/aros-development-team/AROS),
pinned at **`8570536`**.

The submodule is never edited in place. Our work reaches it two ways, and the
split is deliberate.

## Two kinds of change, two mechanisms

| | What | Where it lives | How it gets in |
|---|---|---|---|
| **The port** | 57 files, ~6900 lines — an entire architecture directory that exists nowhere upstream | `aros/arch/m68k-emu68/` in this repository | symlinked into place by `scripts/setup.sh` |
| **Upstream changes** | 11 patches over 20 files (18 modified, 2 added), +362/−24 | `patches/aros/` | applied by `scripts/setup.sh` |

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

Eleven patches, numbered `0001`-`0011` with no gaps, in three kinds. The
distinction is the point of the table: a patch that exists because this target
exists is not the same thing as a defect someone else has, and neither is an
instrument.

Numbers are contiguous on purpose. `setup.sh` applies `[0-9]*.patch` in numeric
order and a hole invites the question of what used to be there; the answer is in
git, not in the numbering. A patch that leaves the series gets renumbered out,
and one that arrives takes the next free number.

### Target enablement — exists because `m68k-emu68` exists

| # | Patch | What it does |
|---|---|---|
| 0001 | `add-the-m68k-emu68-target-to-configure` | Registers an m68k target whose architecture is not `amiga`. Everything else follows from that distinction being expressible. |
| 0002 | `m68k-all-support-an-m68k-that-is-not-an-amiga` | The shared m68k layer assumed Amiga hardware in cache maintenance, dispatch, signalling, task switch and wait. Adds `preserveall.S` and `preserveall_install.c`. |
| 0004 | `dosboot-key-the-planar-boot-image-on-target-arch-not` | The planar boot image was selected on `AROS_TARGET_CPU=m68k`, which also catches an m68k with a chunky framebuffer and no blitter. Tests `AROS_TARGET_ARCH=amiga` instead. |
| 0010 | `m68k-all-do-not-race-the-emu68-exec-backend` | `arch/m68k-all/exec` and `arch/m68k-emu68/exec` both declared `%build_archspecific` for `switch`, `dispatch` and `preparecontext`, writing the same object path. mmake has no notion of one overriding the other: they raced, and a full rebuild reversed the order for the first time on 2026-08-07, linking 66-byte stubs beside a `kernel_cpu.c` that removes their symbols on purpose. The guest jumped to address zero 570 ms in. |

These four are candidates for upstreaming and none can be dropped before that
happens.

### Defects upstream still has

| # | Patch | What it does |
|---|---|---|
| 0003 | `sdcard-initialise-sdcu-softlist-before-addhead-uses` | `NEWLIST` on `sdcu_SoftList` was lost when the driver was derived from `rom/devs/ata`. `AddHead()` on a zeroed list writes through a NULL `lh_Head`, i.e. to address 4 — harmless-looking on the ARM ports that have used this driver, fatal on m68k where address 4 is `AbsExecBase`. |
| 0005 | `raise-the-m68k-default-task-stack-to-match-the-other` | m68k's default task stack was left below what the other targets use. |
| 0006 | `fat-write-cluster-and-size-little-endian` | FAT directory entries are little-endian on disk whatever the host is. The write path stored cluster numbers and sizes native, so a card written here was unreadable elsewhere and, worse, reread wrong. |
| 0008 | `fat-convert-directory-dates-little-endian` | The same defect in `ConvertFATDate`/`ConvertDOSDate`. Converted inside the two functions rather than at the call sites, so there is one place to be right. |
| 0011 | `kernel-avoid-undersized-tlsf-free-blocks` | `tlsf_malloc()` split a block whenever the remainder had room for a `hdr_t`. On a 32-bit target that leaves a four-byte payload, and `INSERT_FREE_BLOCK` then writes its eight-byte `free_node_t` **across the following block's header**. The split now requires room for the header *and* a complete free-list node. |

0006 and 0008 were re-checked against upstream HEAD on 2026-08-13 and the
defect is still there. **We owe all four upstream**, along with two further
sites found the same day and not yet patched here: `rom/filesys/fat/ops.c` line
599 and `rom/filesys/fat/direntry.c` line 242 build cluster numbers from the raw
little-endian fields with no `AROS_LE2WORD`, unlike the `FIRST_FILE_CLUSTER`
macro that does it correctly. Neither is on the boot path.
See [`upstream-candidates.md`](upstream-candidates.md).

**0011 is the one that ended ISSUE-0007.** The heap corruption this port spent
ten days on was the allocator corrupting itself, and every hypothesis about an
external writer -- task switch, `CopyMem`, `lddemon` expunge, an overlap with
Emu68's own pools -- was wrong. Full physical-chain validation caught the
invalid state twice at the same address and operation, at `tlsf_malloc()`'s
exit rather than on entry, which is what identified the allocator as the author.
With it applied and the diagnostics removed, the boot reached Wanderer with
icons in **10 of 10 runs** on a freshly generated card, 38.8-44.5 s.

### Instruments — not fixes

| # | Patch | What it does |
|---|---|---|
| 0007 | `kernel-refuse-to-free-a-pointer-outside-the-heap-and` | Refuses the free and names the caller instead of corrupting the allocator quietly. |
| 0009 | `exec-name-the-caller-that-frees-outside-the-pool` | Reports the caller when a pointer arrives at `FreeVecPooled` that the pool does not own. Has never fired. |

These two are guards rather than traces: they cost a range check on a path that
already fails, and they print only when something is already wrong. They are on
in every build deliberately.

**The one that could not stay on is not in the series.**
`optional-debug-turn-on-tracing-in-dos-lddemon-and-shell.patch` has no number,
so `setup.sh` -- which globs `[0-9]*.patch` -- does not apply it. It sets
`DEBUG 1` in the boot and shell paths, which floods the serial console and
changes boot timing substantially, and the failure it was written for was
timing-sensitive and intermittent. Six series were run on 2026-08-13 whose icons
rate moved 83%, 25%, 25%, 50%, 40%, 8% with **only instrumentation changing**.
A measurement run and a diagnostic run must not be the same build; apply it by
hand with `git -C external/aros apply` when you want it.

### What was dropped, and why it matters

The series was 19 patches at pin `d0370bd`. Eight went at the bump to
`8570536` because upstream has them:

| Was | Upstream |
|---|---|
| `0013`–`0017`, `0019` | imported verbatim from upstream in the first place |
| `0009` (ELF loads from an unfilled buffer) | `2f514b7472` |
| `0004` (synchronous `System()` packet) | `bde1ec0f23` |

The last one is worth remembering rather than just deleting. We patched
`rom/dos/newcliproc.c` so a synchronous `System()` keeps its flags. Upstream
deletes those lines and fixes `arch/m68k-all/dos/bcpl.S`, where the BCPL flag
translation was being applied to every `System()` call instead of only to
genuine BCPL Shell-Segs. Same bug — both descriptions name `AROSMonDrvs` —
and theirs is at the cause. Ours was a symptom patch, and it was masking the
real defect for as long as it was applied.

All ten surviving patches applied to HEAD without a single conflict across 785
upstream commits, which is the evidence that what remains is genuinely ours
rather than upstream with a delay. The eleventh, `0011`, was written after the
bump.

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

## The toolchain is built once, in CI, and reused

Nobody should be compiling gcc. `build-aros.sh` looks for a prebuilt copy before
it compiles anything, in this order:

1. `~/.cache/bellatrix/toolchain/` — a local tarball, written by any build here
   that ended up with a good toolchain.
2. The GitHub release named `toolchain-<digest>`, fetched by `fetch_toolchain()`
   and then cached locally so the download happens once per machine.
3. Only if both miss does it offer to compile, and in a non-interactive shell it
   refuses rather than silently spending hours (`--yes` accepts the cost).

`BELLATRIX_TOOLCHAIN_FETCH=0` keeps step 2 off the network.

The release is produced by `.github/workflows/toolchain.yml`, which runs
`build-aros.sh --toolchain-only` and uploads the tarball plus its `.sha256` to a
release tagged after the digest. It is a *release asset* rather than an Actions
cache on purpose: caches are scoped to a branch and expire, and this is meant to
outlive both.

### What the name means, and why it is portable

```
a88db85e62ede04f - m68k - linux-x86_64 - glibc2.39 .tar.xz
      digest       cpu       host          libc
```

- **digest** — `sha256` of the AROS pin's `config/gcc_def`, `config/binutils_def`
  and `tools/crosstools`, plus any patch of ours that touches them. It
  deliberately ignores our port sources and the rest of the patch series: the
  most expensive thing to build is the thing that changes least, and a digest
  that moved daily would be worthless.
- **cpu** — `${TARGET##*-}`, so `emu68-m68k` contributes `m68k`. See below.
- **host + libc** — the only host coupling is the C library, so an entry is
  usable on any host with that glibc *or newer*; the lookup takes the newest
  compatible one rather than demanding an exact match. gcc resolves its own
  prefix relative to its binary, so the absolute path it was built under does not
  matter and the tarball relocates freely.

Only `crosstools/` travels. Beside it sit wrappers (`<cpu>-<arch>-elf-gcc`,
`aros-ld`) that `configure` generates with the absolute path of the tree they
belong to baked in. They are part of the build, not of the toolchain; packing
them would make the cache portable in name only.

### The same toolchain serves any m68k AROS target

The key is scoped by **CPU**, not by AROS architecture. `emu68-m68k` and
`amiga-m68k` both reduce to `m68k`, so the published
`a88db85e62ede04f-m68k-linux-x86_64-glibc2.39.tar.xz` is the toolchain for both.
This is not a coincidence: AROS gates its crosstools stages per CPU inside one
directory (`.installflag-gcc-<version>-m68k`), so two AROS targets on the same
CPU would build the same compiler into the same place anyway.

`build-aros.sh` cannot yet *point* at another tree, though — its `BUILD` and
`TARGET` are this port's. Giving a second AROS target the same toolchain by hand
is two commands — `configure` writes the tree, the tarball fills in the compiler
it left out:

```bash
cd /somewhere/short && /path/to/external/aros/configure --target=amiga-m68k
tar xJf ~/.cache/bellatrix/toolchain/$(cd /path/to/bellatrix && \
    ./scripts/build-aros.sh --toolchain-key).tar.xz \
    -C /somewhere/short/bin/linux-x86_64/tools/
```

`configure` writes the wrappers, the tarball supplies `crosstools/` beside them,
and `make` skips the toolchain stage entirely. Closing that gap properly is
[`ISSUE-0026`](../AI_context/issues/ISSUE-0026.md).

**Use a short build path.** Not an Amiga issue and not an AROS one: linking
`stdc.library` passes every object on one command line, and a long build-tree
prefix multiplied by hundreds of paths overflows `ARG_MAX` —
`/bin/sh: Argument list too long`. A path like `/tmp/a68k` is fine.

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
