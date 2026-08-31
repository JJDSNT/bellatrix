# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

`AGENTS.md` carries the coding-style, commit and PR conventions. This file covers
the architecture and the traps that only show up after reading several files.

## What this is

AROS/m68k on a Raspberry Pi 3 under Emu68, with no Amiga chipset. Emu68 owns the
bare metal and JITs M68K to AArch64; AROS starts after Emu68 has initialised the
hardware and loaded its m68k ELF from `-initrd`. Both are upstream projects,
vendored as pinned submodules and never edited in place.

## Where the project is right now: the freeze is over

**The feature freeze of 2026-08-17 was lifted on 2026-08-29.** For twelve days
nothing new was added: the rule was that every addition lands on a base whose
speed and stability are not settled, and then has to be re-verified when they
are. That base is settled enough now, and the first thing built on it is the
Rigel chipset integration (`ISSUE-0068`).

Two things follow, and they are not the same thing:

- **The freeze no longer blocks anything.** An issue parked as `backlog` with
  "out of scope under the standing freeze" written in it is no longer parked
  for that reason. Several issues still carry that sentence — `ISSUE-0038`,
  `0040`, `0041`, `0042`, `0044`, `0045`, `0047`, `0053`, `0058`, `0060`,
  `0066`. Those sentences are the record of a decision that has since been
  reversed; read them as history, not as a live gate.
- **The reasoning that produced it still stands.** Speed and stability are
  still what the machine is short of, an addition still has to be re-verified
  when they move, and an issue in this repository is still not permission to
  build the thing it describes. What changed is who decides — that is a call
  per piece of work now, not a blanket rule.

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
- **Development builds keep frame pointers, on purpose.** `build-aros.sh`
  defaults `BELLATRIX_FRAME_POINTERS=1`, which makes `configure` pass
  `-fno-omit-frame-pointer` for the whole target (`patches/aros/0028`). Every
  m68k target upstream omits them; this one does not, because without a frame
  chain `KrnBacktraceFromFrame()` and the crash requester's `Stack trace:`
  section both report nothing at all — a build that cannot say where it broke
  costs more than the register it saves. `BELLATRIX_FRAME_POINTERS=0` restores
  upstream's choice.
  - It is read by `configure`, so **changing it reconfigures and rebuilds the
    tree** (the toolchain is kept). `build-aros.sh --status` reports the tree's
    setting and whether the next build will flip it.
  - **Numbers taken with it on do not compare with `out/boot-timing.jsonl`**,
    whose 153 runs predate it. Any performance comparison has to fix the
    setting on both sides.
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
  - **A cached copy is looked for before anything is compiled**: first
    `~/.cache/bellatrix/toolchain/`, then the `toolchain-<digest>` release
    published by CI. A build only reaches the prompt when neither has one that
    runs here. `BELLATRIX_TOOLCHAIN_FETCH=0` keeps it off the network.
  - Only `crosstools/` is cached, never the whole of `bin/<host>/tools/`: the
    wrappers beside it embed the absolute path of the tree that configured them,
    so they belong to the build and `configure` writes them again.
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

### Deleting a patch needs one step first

`setup.sh` learns everything from the patches that are *there*. Delete one and
it stops knowing about the files that patch touched — so it never clears their
`skip-worktree` bit, `git reset --hard` silently skips them, and the deleted
patch stays applied forever. `--reset` then reports `dirty` and cannot fix it,
because fixing it is the thing it no longer knows how to do.

So **un-apply the patch before deleting it**, or afterwards clean up by hand:

```bash
git -C external/<name> ls-files -v | grep '^S'      # what is still marked
git -C external/<name> update-index --no-skip-worktree <files>
./scripts/setup.sh --reset
```

Files the patch *created* need the same treatment: they are untracked and named
in `.git/modules/external/<name>/info/exclude`, so nothing sees them. Delete
them and their exclude lines.

## Boot chain and the IRQ bridge

QEMU `raspi3b` → `-kernel Emu68.img` + `-dtb` → `-initrd aros-*.elf` →
`-drive if=sd` mounts as `SDCARD0P0:` → `S:Startup-Sequence` → Shell → Wanderer.

Physical BCM283x interrupts do not take the Amiga Paula path. The chain is:

```
ARM peripheral → Emu68 stores level 6 into INTF.IPL → m68k level 6
→ Platform_Autovector() → ARM interrupt-controller Dispatch()
→ krnRunIRQHandlers() → driver handler
```

The level is handed straight to the CPU as an IPL — the same field an external
interrupt controller drives on PiStorm — and the arbitration honours it against
the SR mask like a real 68k. **There is nothing for this port to arm and
nothing to acknowledge on the bridge**: Emu68 drops the level as it takes the
exception, and the SR mask keeps it from re-entering until our RTE. Only the
peripheral that fired has to be acknowledged, which `Dispatch()` does.
`KrnCli()`/`KrnSti()` are *not* the physical IRQ gate on this port.

This replaced an emulated Paula, where Emu68 raised level 6 only when the guest
`INTENA` shadow held both `INTEN` and `EXTER` and pending state cleared through
`INTREQ` — one page fault per arm and per acknowledge. That path still exists in
Emu68 and is the right answer once something really owns those registers; it is
simply not what a machine with no chipset needs. See
`patches/emu68/0002-deliver-host-interrupts-as-an-ipl-not-through-a-shadow.patch`
and the comment at `aros/arch/m68k-emu68/platform/platform.c:20-44`.
`docs/irq.md` compares the three possible delivery mechanisms (shadow
registers, MOVEC, PiStorm's IRQ line); its "The path" section still describes
the shadow and carries a correction header saying so.

Neither state is the destination. `docs/New_emu68.md` §3 and §14 split this into
two interrupt domains — platform interrupts keep the `INTF.ARM` → level 6 path,
chipset interrupts belong to Rigel — and delete `INT_shadow` outright, routing
`$DFF09A` to Rigel through a generic bus hook. Anything reasoning about who owns
INTENA/INTREQ should be written against that, not against the current dormancy.

The `nocomposition` boot argument is currently required to see anything on the
framebuffer.

## The chipset is a boot argument, not a build option

`CONFIG_RIGEL` says whether the image *carries* Rigel. Whether the machine that
boots *has* it is the bare word `rigel` on the kernel command line -- the single
line in `cmdline.txt`, or `BELLATRIX_RIGEL=0/1` for `run.sh` and
`make-sdcard.sh`. Absence is the off case; there is no `rigel=0`.

Both halves read it and neither tells the other: `src/machine/options.c` for the
host (address map, chipset init, cores 2 and 3, how `STOP` waits) and
`arch/m68k-emu68/boot/boot.c` for the guest (one heap, or Fast plus a separate
Chip pool). They can only disagree if `rigel` is asked for on a `CONFIG_RIGEL=0`
image -- and Emu68 then blanks the word out of the guest's copy of the command
line (`patches/emu68/0026`), so the guest agrees with the machine rather than
with the request. **Both print which machine they got** -- the two compositions
differ in what `AllocMem(MEMF_CHIP)` returns and in nothing else visible, so
read those two lines before concluding anything about memory.

**Under QEMU the chipset needs `bellatrix.chipdiv=N` to be usable at all.**
Chipset time is a function of the wall clock and of nothing else, so a host
that cannot deliver 3546895 colour clocks a second does not get a slower
chipset: it gets a core saturated at 100%, four fifths of the requested colour
clocks discarded at the catch-up cap, and a 5.9 ms lock hold in front of every
CPU access to the classic domain. Measured, and the boot stalls in the graphics
drivers because of it. `bellatrix.chipdiv=N` scales the demand to what the host
can supply and nothing else -- a colour clock still costs a colour clock. The
option defaults to 1, which is right on a Pi 3 and wrong nowhere else, but
`run.sh` defaults it to 8 whenever it turns the chipset on, because that script
is the QEMU path and never touches a card. `BELLATRIX_CHIPDIV=1` reproduces
hardware there. ISSUE-0075 has both measurements.

**A property on `/emu68` is not a channel to the guest.** `dt_add_property()`
edits Emu68's own parsed tree; the guest receives a byte copy of the original
blob, which has no `/emu68` node in it. Four attempts have now been made -- see
`patches/emu68/0007`. What reaches the guest is an in-place correction of the
copy, which is what patch 0007 does for `/memory` and patch 0026 for
`/chosen/bootargs`. `boot.c`'s `/emu68/host-mem` reader is dead for this reason
and is kept only as a no-cost guard.

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

## Probes that report from inside a boot

`BELLATRIX_BOOT_TEST=<name>` makes `make-sdcard.sh` insert
`Execute "S:<name>"` into `S:Startup-Sequence`, and `tests/gl/` holds the
scripts. They report by redirecting to `DEBUG:`, which reaches the serial
line and therefore the `run.sh` log.

Two things had to be true for that to work, and both were false until
2026-08-24 -- for months, silently, across seven scripts:

- `DEVS:DOSDrivers/DEBUG` names `L:debug-handler`, and nothing built it. The
  target is `workbench-fs-debug`.
- the probe is inserted **after** `S:Startup-Sequence` mounts
  `DEVS:DOSDrivers`, not before. It used to be anchored twenty lines earlier,
  so it ran before `DEBUG:` existed as a device.

**A probe that prints nothing and a probe that never ran look identical**, and
reading the first as the second is how an investigation goes hours in the
wrong direction. If a script goes quiet, check the handler and the anchor
before concluding anything about what it was measuring.
`BELLATRIX_BOOT_TEST_LATE=1` moves it to just before Wanderer for anything
that needs a finished system.

## Measuring a boot

The AROS boot to desktop is **timing-sensitive and intermittent**. Never conclude
from a single run: use at least 3 serial runs per configuration, alternating
configurations, on an idle machine (`ps -eo cmd | grep -c '[q]emu-system'` must
be 0 first). Concurrent QEMU instances change the outcome. Capture objectively
with `screendump` and compare dominant colour — the Emu68 logo screen and the
Workbench screen are both grey.
