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

**D — split the asset.** The same seam AROS maintains: the aarch64 side
(firmware, Emu68, `config.txt`, `cmdline.txt`) builds in minutes and is
CI-friendly; the m68k side is the expensive one. Two assets, `-boot` and
`-system`, let the cheap half be automated without waiting on the other.
Composes with A, B or C rather than competing with them.

## Decision in force

Start with **A**. The script is what B and C would call anyway — only the caller
changes — so it is not throwaway work, and once it exists a single measurement
of a cold `build-aros.sh full` decides between B and C on evidence.

Two properties belong in it from the start:

- **A version stamped inside the archive** — tag, commit and submodule pins, so
  a card identifies itself long after the download is forgotten.
- **A name with room in it** — `bellatrix-<tag>-pi3-system.tar.xz`, following the
  AROS convention and leaving `-boot` free for design D.

A release is correct when the archive extracts at the root of a FAT32 partition
and boots a Pi 3, it carries its `.sha256`, it names the commit it was built
from, and the build refused to run from a `dirty` submodule state.
