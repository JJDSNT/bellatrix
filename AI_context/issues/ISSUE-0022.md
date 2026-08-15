---
id: ISSUE-0022
title: "Publish the --pack archive as a GitHub release asset"
status: backlog
priority: low
type: infra
owner: agent
created_at: 2026-08-15
updated_at: 2026-08-15
tags:
  - release
  - packaging
  - ci
blockers:
  -
related_files:
  - docs/release.md
  - scripts/make-sdcard.sh
  - scripts/build-aros.sh
---

# Summary

`scripts/make-sdcard.sh --pack` produces the archive a user needs, but it exists
only on the machine that built it. This issue tracks giving it a URL.

The survey behind the decision — what the artifact is, how Emu68 and AROS
package theirs, the mechanism this repository had before the reset, the measured
build costs, and the four designs available — is in
[`docs/release.md`](../../docs/release.md). This file tracks only the work.

# Problem

Three things are missing between `out/aros/bellatrix-pi3.tar.xz` and a download
link: a name that says which build it is, a checksum, and somewhere to put it.

The obstacle is cost. The legacy release asset was an aarch64 kernel image,
minutes of CI; this one depends on `build-aros.sh full`, which builds an m68k
cross toolchain from source first. A 7.9 GB build tree and a from-source
toolchain inside a six-hour hosted job, with source fetches happening during the
build, is the constraint every design has to answer.

# Goal

A tagged release carries a card archive that a user can download, verify and
unpack onto a FAT32 partition without knowing anything about how it was built.

# What was done

Nothing implemented. The survey is written up in `docs/release.md`, including
the decision to start from the workstation script rather than from CI.

# The ground, as surveyed 2026-08-15

Three findings that constrain the script before a line of it is written.

**The verification gate had work to do on day one, and it did it.** When this
was written `./scripts/setup.sh --verify` exited 1 on
`0027-usb-remove-mouse-hot-path-diagnostics.patch`, which meant the tree could
not be reproduced from a commit and a strict gate would have blocked the first
release. Resolved on 2026-08-15: 0027 was written to remove the traces 0026 had
added but kept `DoIO(...)` as context, where 0026 had turned that same line into
`ioerr = DoIO(...)`. The series now applies and reproduces the working tree
exactly. The gate can be strict.

**There is no version to derive.** `git describe --tags` returns nothing on
`main` — the existing tags are not reachable after the reset. The version has to
be an argument, not an inference.

**`--pack` hardcodes its output name.** `make-sdcard.sh` computes
`ARCHIVE="$(dirname "$OUT")/bellatrix-pi3.tar.xz"`; `--out` only applies to the
image path. Naming an asset per release needs `--out` honoured on the pack path
too.

# The script

```bash
./scripts/release.sh v0.1.0 [--dry-run] [--skip-build] [--draft] [--notes FILE]
```

**1. Preflight.** Clean working tree; `setup.sh --verify` passing; `gh auth
status` healthy; the tag either absent or being re-uploaded to. Nothing is built
before this — failing after a two-hour build because of an uncommitted file is
the worst available outcome.

**2. Build**, unless `--skip-build`: `build.sh`, then `build-aros.sh full`.

**3. Stamp the version — in `make-sdcard.sh`, not here.** That script already
generates `config.txt` and `cmdline.txt` into the staging directory, for the
reason its own comment gives: they name what the script has just copied, and the
two drifting apart is a card that stops with no message worth reading. A
`version.txt` carrying tag, commit, date and submodule pins is the same
argument. Writing it there makes every card self-identifying, the QEMU one
included, and keeps `release.sh` out of another script's staging directory — it
only passes the tag in through the environment.

**4. Name and checksum.** `out/release/bellatrix-<tag>-pi3.tar.xz` for the card,
plus the two increments under the names `config.txt` declares, and a `.sha256`
beside each.

**5. Verify the archive before publishing.** The part that makes a release
trustworthy, and the principle the legacy workflow already applied. Concretely:
paths start with `./`, none absolute and none containing `..`; `bootcode.bin`,
`start.elf`, `Emu68.img.gz`, the AROS ELF, at least one `bcm2710-*.dtb`,
`AROS.boot`, `C/`, `S/Startup-Sequence`, `Libs/` and `Devs/` are present; and
`Devs/DOSDrivers/AUX` survives — the mtools filter strips it from the image and
it has already gone missing from the archive once. Finally the names on the
`kernel=` and `initramfs` lines of `config.txt` must exist inside the archive,
which is exactly the failure that produces seven blinks and no console. This
function is reusable as-is when the build moves to CI.

**6. Publish.** `gh release view` decides between `create` and
`upload --clobber`; pre-release by tag suffix; notes from
`.github/release-notes/<tag>.md` when present, `--generate-notes` otherwise.
`--dry-run` stops here and prints what it would have done.

# Isolating the expensive build

The reason to split a release is not asset size, it is that one of the things on
a card costs hours to build and the others do not. What is published, what gets
rebuilt and why mixing versions is allowed are in
[`docs/release.md`](../../docs/release.md#what-a-release-publishes); what matters
here is the mechanism.

**Three assets, one of them a package.** `bellatrix-<tag>-pi3.tar.xz` is the
whole card, for a first installation. `Emu68.img.gz` (748 KB) and
`aros-emu68-m68k.elf` (1.19 MB) ship as loose files under the exact names the
card expects, because they are precisely the two files `config.txt` names and an
update has to be a copy, never a rename.

**Identity by content.** Each input is a pure function of tracked paths, so a
digest decides whether it has to be rebuilt at all:

```bash
# the AROS tree the card boots from: pin + patch series, deliberately NOT
# aros/, since the port's modules link into the ELF instead of shipping as files
{ git rev-parse HEAD:patches/aros
  git -C external/aros rev-parse HEAD; } | sha256sum

# the kernel ELF: the same, plus this project's own sources
{ git rev-parse HEAD:aros
  git rev-parse HEAD:patches/aros
  git -C external/aros rev-parse HEAD; } | sha256sum

# Emu68
{ git rev-parse HEAD:patches/emu68
  git -C external/emu68 rev-parse HEAD; } | sha256sum
```

No value is quoted here on purpose: the ELF digest moves with every commit that
touches `aros/` or the patch series, so a number written into a document is
stale by the next day. The current ones are on the card, in `version.txt`, and
in the notes of whichever release produced it.

The digests are published as information, not as a gate: a newer ELF meeting an
older volume is a supported state, because that is what `docs/Compat.md` asks
for. What the notes owe the reader is whether the system digest moved, since
that is what decides if dropping in the two files is enough.

**The toolchain cache.** Two facts were measured rather than assumed: the
toolchain relocates (gcc resolves its prefix relative to the binary, verified by
compiling from a copy at another absolute path), and it needs glibc >= 2.38 and
nothing else. So the cache key is `<digest>-<arch>-glibc<version>`, any workflow
using it has to say `runs-on: ubuntu-24.04`, and packing/restoring cost 39 s and
14 s for 184 MB.

CI is the consumer that decides the shape. `actions/cache` alone cannot serve
it: 7-day eviction, a 10 GB repository budget, and branch scoping outside the
default branch mean a monthly release finds no cache and pays for gcc every
time. A release asset has none of those limits. They are tiers, with the durable
one as the floor:

```
local cache -> actions/cache (CI only) -> release asset -> build from source
```

The toolchain is built by CI, in a workflow dispatched by hand when the digest
moves, which publishes the asset and seeds the cache. Nobody has to trust a
binary from a laptop, and the GPL obligation gets a public log naming gcc 6.5.0
and binutils 2.32 at the pinned AROS commit.

# What is left

- Measure `build-aros.sh full` twice: incremental (what running it episodically
  costs here) and cold with the toolchain present (what a runner would pay).
  With the disk figure, those decide whether a hosted runner can build a release
  at all, or whether it has to be self-hosted.
- Write `scripts/release.sh` along the six steps above.
- Teach `make-sdcard.sh` to honour `--out` on the pack path and to write
  `version.txt` into the staging directory.
- Toolchain cache: local tier first, then the CI tiers, sharing one key.
- Check at publication that the names in `config.txt` match the two increments
  being published.
- Give the boot-to-kernel boundary a voice: `EMU68_BOOT_ABI` read by Emu68
  rather than only declared by AROS. The volume boundary stays ungated.

# Decisions taken

- Start from a workstation script (design A in `docs/release.md`), not from CI.
  The script is what a workflow would call anyway, so it is not throwaway work.
- Keep `xz`. It is 19.2 MB against 28.0 MB for `tar.bz2`, and matching the AROS
  convention buys nothing else.
- Keep the archive's `./…` paths with no top-level directory. Identity goes in
  the filename and in a stamped file, never in a wrapping directory.
- Identity is a property of the card, not of the release: `version.txt` is
  written by `make-sdcard.sh` for every card it stages.
- No `--allow-dirty` escape hatch. A valve like that becomes the normal path the
  first time someone is in a hurry.
- Three assets: the full card as an archive, and the two files `config.txt`
  names as loose files under their exact names. No `-boot`/`-system` split of
  the card -- the increments are the update path, the archive is the install.
- Mixing versions is supported, not gated. `docs/Compat.md` asks the resident
  system to boot volumes it was never built with; a release that refused the
  same thing on its own card would contradict the project.
- The toolchain cache has a durable floor (a release asset) with
  `actions/cache` as a fast tier above it, never as the only tier.

# Open questions

- **Does the script create the tag?** Minor, and deferred. Preference: create the
  annotated tag at HEAD when missing, push only after confirmation, since
  pushing a tag is an outward-facing act and not a side effect of a build.
- **Is a hosted runner viable?** Waiting on the two `full` measurements above.

# Acceptance criteria

- [ ] a tagged release carries the archive and its `.sha256`;
- [ ] the archive extracts at the root of a FAT32 partition and boots a Pi 3;
- [ ] the archive names the commit it was built from;
- [ ] the build refuses to publish from a `dirty` submodule state.

# Notes

`gh` on the workstation is already authenticated with the `repo` scope, so
design A needs no new credentials.

# Execution log

- 2026-08-15 — survey written to `docs/release.md`; issue opened to track the
  implementation. No code written.
- 2026-08-15 — script designed against the actual state of the tree: the six
  steps, the archive checks, and the three findings above (`--verify` failing,
  no reachable tag, `--pack` ignoring `--out`). Still no code written.
- 2026-08-15 — AROS series drift resolved (0027's context did not account for
  0026 having changed the same line); `setup.sh --verify` passes again, so the
  release gate can be strict.
- 2026-08-15 — `build-aros.sh` taught not to throw the toolchain away, to refuse
  a surprise toolchain build when there is no terminal to ask, and to answer
  `--status` without building. ccache passed to configure on fresh trees only.
- 2026-08-15 — the cross toolchain identified as a fourth expensive thing, of a
  different kind: a build input, host-specific, 653 MB installed, with the
  narrowest identity of all and destroyed by `build-aros.sh clean`.
- 2026-08-15 — the split resolved into three artifacts, not two, once it was
  verified that the port's modules link into the kernel ELF and never reach the
  card as files: the expensive system tree does not depend on this project's
  sources at all. Identity-by-content added as the mechanism. Naming, ownership
  of `config.txt` and mismatch detection left open.
