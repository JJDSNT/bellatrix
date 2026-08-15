# Release

How a build becomes something a person can download. The artifact exists today;
the link does not. This document records what the artifact is, what the two
upstream projects do with theirs, what the real constraints are, and which
designs those constraints leave standing.

## The artifact

`scripts/make-sdcard.sh --pack` produces `out/aros/bellatrix-pi3.tar.xz`: the Pi
firmware, `Emu68.img.gz`, the AROS m68k ELF, the generated `config.txt` and
`cmdline.txt`, and the AROS distribution tree. Paths are written as `./…` with
no top-level directory, so

```bash
tar -xJf bellatrix-pi3.tar.xz -C /media/you/BOOT
```

lands the files at the root of a FAT32 partition, which is what makes the card
bootable. Any naming or versioning scheme has to preserve that: identity belongs
in the filename and in a stamped file inside the archive, never in a wrapping
directory.

The archive carries no identity of its own — no tag, no commit, no submodule
pins. That is the first gap a release has to close.

## What the reference projects do

Neither Emu68 nor AROS packages a release from its build system. Both stage
files with the build system and archive them in CI.

**Emu68** — `external/emu68/.github/workflows/cmake.yml`. `cmake --install .`
stages the payload; `zip -r ./Emu68-<targ>.zip .` runs from inside the staging
directory, so the archive's paths are relative and extract at the root of the
boot partition; `softprops/action-gh-release` attaches it when the ref is a tag.
One zip per variant: `Emu68-raspi.zip`, `Emu68-pistorm.zip`,
`Emu68-pistorm-classic.zip`.

**AROS** — `scripts/azure/templates/steps-core.yml:160-200`, in the AROS tree.
Two properties worth borrowing:

- *The format is a build parameter.* `arosbuild.packagefmt` selects `zip`,
  `tar.bz2` or `lha`, each with its own branch, and an `.md5` is written beside
  the archive. The values live in the pipeline variables, not in the tree.
- *Boot and system are packaged separately.* `bootpackage`/`bootpackagefmt`
  archive `distfiles-boot`; `package`/`packagefmt` archive `distfiles`. The two
  halves have different build cycles, so they are different files.

The nightly page composes its links as
`AROS-<YYYYMMDD>-<package>.tar.bz2` under
`sourceforge.net/projects/arosdev/files/nightly2/<date>/<group>/`
(`scripts/azure/gen-downloads.py:182`) — the origin of the `.tar.bz2` in
circulation. The mmake side contributes only staging:
`distfiles-raspi-aarch64le` ends in `cp -R $(AROSDIR) $(DISTDIR)/`
(`arch/aarch64-raspi/boot/mmakefile.src:232`).

Not every AROS asset is an archive. `AROS-<date>-raspi-aarch64-qemu.img.xz` is a
raw disk image, xz-compressed, for QEMU rather than for a card; the `qemu`
package bypasses `packagefmt` entirely.

AROS also caches its built toolchain as a tarball between runs
(`scripts/azure/azure-pipelines.yml:81-101`), which is the standard answer to
the cost problem below.

## The mechanism this project already had

`git show legacy:.github/workflows/release-images.yml` is a working
implementation, and the three published releases (`v0.0.0`, `0.0.1`,
`v0.0.2-rc1`) came from it. Its parts:

- triggers on `workflow_dispatch` and on `push` of tags `v*` or `[0-9]*`;
- `permissions: contents: write`, so `${{ github.token }}` can publish;
- a build matrix, one artifact per variant, gathered by a `publish-release` job
  with `download-artifact` and `merge-multiple: true`;
- publication that is idempotent by construction:

```bash
if gh release view "$TAG_NAME" >/dev/null 2>&1; then
  gh release upload "$TAG_NAME" dist/* --clobber
else
  gh release create "$TAG_NAME" dist/* --title "$TAG_NAME" --generate-notes ...
fi
```

- pre-release by tag suffix — `*-rc*`, `*-beta*`, `*-test*` get
  `--prerelease --latest=false`;
- hand-written notes at `.github/release-notes/<tag>.md` when present,
  `--generate-notes` otherwise;
- a `sha256sum` beside every asset;
- assertions before publication. The legacy job grepped the built image for a
  backend marker and the `CMakeCache.txt` for the flags it was supposed to have
  been configured with, and failed rather than publish a mismatch. The principle
  generalises: a release must refuse to publish an artifact it cannot prove is
  the one it meant to build.

`gh` on the workstation is authenticated with the `repo` scope, so publishing by
hand needs no new credentials.

## Constraints

The difference from the legacy situation is cost, not plumbing. There the asset
was an aarch64 kernel image, minutes of CI. Here it depends on
`build-aros.sh full`, which builds an m68k cross toolchain from source before it
builds AROS.

Sizes in this tree, measured 2026-08-15:

| | |
|---|---|
| `out/build/aros` | 7.9 GB |
| distribution tree `bin/emu68-m68k/AROS` | 370 MB |
| `bellatrix-pi3.tar.xz` (`xz -9`) | 20,131,612 B (19.2 MB) |
| the same content as `tar.bz2 -9` | 29,317,162 B (28.0 MB), **+46%** |
| GitHub per-asset limit | 2 GB |
| GitHub-hosted job limit | 6 h |

The asset size is irrelevant; the build size is not. A hosted runner has to fit
a 7.9 GB build tree and a from-source toolchain inside six hours, and
`build-aros.sh full` fetches external sources while it runs — a release build
that depends on the network at build time is fragile by construction.
`build-aros.sh` is also deliberately serial, so the usual way of buying time
back on a four-core runner is not available.

On format, `xz` stays: 19.2 MB against 28.0 MB, and any host that can write a
FAT32 card can read either. Matching AROS's `.tar.bz2` would buy convention and
nothing else.

## Designs

**A — publish from the workstation.** A `scripts/release.sh` that refuses unless
`setup.sh --verify` reports `applied`, runs `--pack`, names and checksums the
asset, and calls the legacy publication logic. Costs nothing, and the published
asset is byte-for-byte the one tested on the Pi. Not reproducible from a clean
checkout.

**B — the legacy workflow, ported.** A tag produces the link with no human in
the loop. Needs three things the legacy workflow did not: a cache for the m68k
toolchain, disk reclamation on the runner, and tolerance for source fetches
during the build.

**C — self-hosted runner.** Workflow B, running where the toolchain and ccache
already exist. Keeps "push a tag, get a link" without rebuilding the toolchain,
at the price of depending on one machine being up.

**D — split the asset.** The seam AROS maintains between its boot and system
packages exists here too, and cuts in three rather than two. Composes with A, B
or C rather than competing with them; see below.

## The cross toolchain

Before the three artifacts, the thing that gates all of them. Building anything
m68k needs `m68k-aros-gcc`, and if it is not there the build makes it from
source — so "the lean build takes minutes" is only true on a machine that has
already paid for the toolchain once.

`out/build/aros/bin/linux-x86_64/tools/crosstools` is **653 MB** installed:
binutils 2.32 and gcc 6.5.0 for `m68k-aros`, plus gmp, isl, mpc and mpfr. As a
`tar.xz -3` it is **184 MB**.

Its identity is narrower than everything else's, and lives entirely inside the
AROS pin:

```bash
{ git -C external/aros rev-parse HEAD:config/gcc_def        # 6.5.0
  git -C external/aros rev-parse HEAD:config/binutils_def   # 2.32
  git -C external/aros rev-parse HEAD:tools/crosstools; } | sha256sum
```

That digest does not move when this project's sources change, nor when the patch
series changes, nor on most bumps of the AROS pin itself. The most expensive
thing to build is the thing that changes least.

`build-aros.sh clean` used to destroy it, preserving instead the ~110 MB of
downloaded tarballs on the grounds that re-fetching them was the slowest part of
starting over. That was backwards — the download is minutes, gcc is hours — and
is fixed: `clean` keeps the toolchain, `distclean` is the verb that drops it.

### It can be moved

Two properties decide whether a cache is possible at all, and both were
measured rather than assumed:

- **It relocates.** gcc resolves its own prefix relative to the binary, so a
  toolchain built under one absolute path compiles from another. Verified by
  copying `crosstools` elsewhere and compiling an object with it.
- **It is bound to the host's C library**, needing **glibc ≥ 2.38** and nothing
  else. That runs on Ubuntu 24.04 (2.39) and later, and not on 22.04 or Debian
  12 — so a workflow using a cached toolchain has to say `runs-on: ubuntu-24.04`
  explicitly, and the cache key has to carry the host tag:

```
a88db85e62ed-linux-x86_64-glibc2.38
```

Packing costs 39 s and produces 184 MB; restoring costs 14 s. Against hours,
both are noise.

### Where a cached copy lives

CI is the consumer that matters — a release built by hand on one workstation
does not need any of this. That ordering rules out using GitHub's own cache
alone: `actions/cache` restores fastest and costs nothing, but entries unused
for **7 days are evicted**, the repository budget is **10 GB** shared with
everything else, and caches are branch-scoped except those made on the default
branch. With a monthly release cadence the cache is gone by the time it is
needed, and every release pays for gcc.

A release asset has none of those properties: permanent, not branch-scoped,
fetched with the `GITHUB_TOKEN` the runner already has, 184 MB against a 2 GB
limit. It is slow next to the native cache and irrelevant next to compiling.

So they are tiers, not alternatives, and the durable one is the floor:

```
local cache → actions/cache (CI only) → release asset → build from source
```

The toolchain is built **by CI**, in a workflow dispatched by hand when the
digest changes, which publishes the asset and seeds the cache. That is not
ceremony: it means nobody has to trust a binary that came off a laptop, and it
gives the GPL obligation a public log naming the sources — gcc 6.5.0 and
binutils 2.32, fetched by the AROS build at the pinned commit.

This is a build input, not a card component — a different kind of artifact from
the three below, and the one that governs the cost of all of them.

## What a release publishes

Three assets, and only the first is a package:

| asset | what it is | size | when it is wanted |
|---|---|---|---|
| `bellatrix-<tag>-pi3.tar.xz` | the whole card | 19 MB | first installation |
| `Emu68.img.gz` | the aarch64 kernel | 748 KB | update in place |
| `aros-emu68-m68k.elf` | the m68k system | 1.19 MB | update in place |

The two increments are not an arbitrary cut of the card. They are **exactly the
two files `config.txt` names** — the boundary the boot already declares:

```ini
kernel=Emu68.img.gz
initramfs aros-emu68-m68k.elf
```

So they ship as loose files rather than archives, under the exact names the card
expects, because updating has to be a copy and never a rename. GitHub scopes
assets per release, so the same name across releases does not collide; the
version is the page it came from. Compressing the ELF would take it from 1.19 MB
to 415 KB and cost the person a tool to unpack it — not a trade worth making for
800 KB.

## What a release rebuilds

Publishing three things does not mean building three things. The card is made of
inputs with different costs and different rates of change, and that is what
makes an update cheap. Every cost below assumes the toolchain above exists:

| input | identity derives from | built by | cost |
|---|---|---|---|
| firmware, DTBs, `Emu68.img.gz` | `patches/emu68`, Emu68 pin | `build.sh` | minutes |
| `aros-emu68-m68k.elf` | AROS pin, `patches/aros`, `aros/` | `build-aros.sh`, lean | minutes |
| the AROS tree the card boots from | AROS pin, `patches/aros` | `build-aros.sh full` | hours |

The third row is the point. **The system tree does not derive from this
project's own sources.** The port's modules link into the kernel ELF rather than
shipping as files: a name search across the distribution tree finds nothing of
`emu68` or `sdcard` under `C/`, `Libs/`, `Devs/` or `Classes/` —
`Devs/Drivers/` carries only the generic `gallium`, `hdaudio`, `i2c` and
`softpipe`, and the port's `sdcard` appears solely as headers under
`Developer/`, which the card does not carry.

So everyday work on the port moves the ELF's identity and leaves the system's
untouched. The hours are spent when the AROS pin or its patch series moves, not
on the work that actually happens most days — and a digest of those inputs turns
"do not rebuild AROS" from a judgement call into a check.

## Mixing versions is allowed

A card assembled from one release and updated from another is a supported state,
not a hazard to be gated. It follows from what the project is for:
[`Compat.md`](Compat.md) asks the resident system to boot userlands it was never
built alongside, AmigaOS included. A release that refused to let a newer ELF meet
an older volume would be contradicting that on its own card.

What a release owes the reader is therefore information, never a refusal:

- the three identity digests, in the notes and in a `version.txt` inside the
  full archive, so a card can say what it is;
- a plain statement of when an in-place update is enough. The system digest
  answers it: unchanged since the release someone installed, and the two loose
  files drop straight in; changed, and the full archive is the honest route,
  because libraries, Zune classes and the commands in `C:` are files on the card
  and no new ELF brings them along. That trap is old — `CLAUDE.md` records the
  months when patches to module code silently never reached what booted.

Two further consequences of the shape:

- **`config.txt` ships only in the full archive**, and it names both increments.
  Those two names are a contract: a release that renames either file, or adds a
  boot argument, breaks in-place updates silently. Checking that the names in
  `config.txt` match the assets being published is cheap and belongs in the
  release script.
- **The boot-to-kernel boundary is mute.** `EMU68_BOOT_ABI`
  (`aros/arch/m68k-emu68/boot/boot.h:7`, published at `boot.c:645`) is declared
  by AROS and read by nobody: no reference exists in `patches/emu68` or in
  `external/emu68/src`. Unlike the volume boundary, this one is a genuine
  handshake that should exist — Emu68 reading the number it is handed is the
  real protection, and a patch away.

## Decision in force

Start with **A**. The script is what B and C would call anyway — only the caller
changes — so it is not throwaway work, and once it exists a single measurement
of a cold `build-aros.sh full` decides between B and C on evidence.

Two properties belong in it from the start:

- **A version stamped inside the archive** — tag, commit, submodule pins and the
  three digests, so a card identifies itself long after the download is
  forgotten.
- **The exact filenames `config.txt` declares** for the two increments, checked
  against it at publication.

A release is correct when the archive extracts at the root of a FAT32 partition
and boots a Pi 3, it carries its `.sha256`, it names the commit it was built
from, and the build refused to run from a `dirty` submodule state.

What is published, and what makes an update cheap, is settled. Open: whether the
script creates and pushes the tag, and two measurements that decide whether a
hosted runner can build a release at all — how long `build-aros.sh full` takes
with the toolchain present, and how much disk it wants against the runner's ~14
GB. See ISSUE-0022.
