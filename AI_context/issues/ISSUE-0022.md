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

**4. Name and checksum.** `out/release/bellatrix-<tag>-pi3-system.tar.xz` and
its `.sha256`.

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

The reason to split the release is not asset size, it is that one of the three
things on a card costs hours to build and the other two do not. The
decomposition, the verification behind it and its consequences are in
[`docs/release.md`](../../docs/release.md#the-three-artifacts); what matters here
is the mechanism that makes the isolation real.

**The toolchain gates everything.** `m68k-aros-gcc` is built from source when
absent, so no build cost quoted anywhere is real on a machine that has not paid
for it once: 653 MB installed, 184 MB packed, and its own digest from
`config/gcc_def`, `config/binutils_def` and `tools/crosstools` inside the AROS
pin. It is a build input rather than a card component, and it is host-specific,
so it is never published to users — but it is the first thing a pipeline has to
cache. See [`docs/release.md`](../../docs/release.md#the-cross-toolchain).

**Identity by content.** Each artifact is a pure function of tracked inputs, so
each can carry a digest of them and be rebuilt only when that digest moves:

```bash
# system: AROS pin + its patch series -- deliberately NOT aros/, since the
# port's modules link into the kernel ELF instead of shipping as files
{ git rev-parse HEAD:patches/aros
  git -C external/aros rev-parse HEAD; } | sha256sum

# kernel: the same, plus this project's own sources
{ git rev-parse HEAD:aros
  git rev-parse HEAD:patches/aros
  git -C external/aros rev-parse HEAD; } | sha256sum   # 2ea9677bf5a2

# boot
{ git rev-parse HEAD:patches/emu68
  git -C external/emu68 rev-parse HEAD; } | sha256sum  # 21cbfe2f9945
```

With that, "do not rebuild AROS" stops being a judgement call and becomes a
check: if the digest has not moved, the release points at the asset already
published instead of rebuilding and re-uploading it. Putting the digest in the
filename gives deduplication and traceability at the same time.

# What is left

- Write `scripts/release.sh` along the six steps above.
- Teach `make-sdcard.sh` to honour `--out` on the pack path and to write
  `version.txt` into the staging directory.
- Decide the three artifacts' names, and which one carries `config.txt`.
- Make the system package exclude the kernel ELF it currently contains.
- Give the two compatibility boundaries a voice — at minimum a check at pack
  time; ideally `EMU68_BOOT_ABI` actually read by the side it is declared to.
- Decide where a cached copy of the toolchain lives — CI cache, or a tarball in
  object storage keyed by its digest. (`clean` no longer deletes it: c59cabc.)
- Measure one cold `build-aros.sh full` — that number decides between hosted CI
  and a self-hosted runner. Measure it with and without the toolchain present;
  they are different questions.

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

# Open questions

Being refined; the decomposition into three artifacts is settled, the way they
are published is not.

- **Does the script create the tag?** Preference: create the annotated tag at
  HEAD when it is missing, but push it only after confirmation — pushing a tag
  is an outward-facing action and should not happen as the side effect of a
  build script.
- **How are the three artifacts named and versioned?** One repository tag across
  all three, or each carrying its own content digest, or both. A single tag is
  simpler to talk about; digests are what make the reuse check work.
- **Which artifact carries `config.txt`?** It belongs to boot by content, but it
  names the kernel ELF, so the boot package cannot be validated on its own.
- **How is a mismatched set detected?** Three artifacts give eight combinations
  and no current mechanism notices a wrong one. Cheap answer: `version.txt` plus
  a check at pack time. Real answer: the ABI number gets read.

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
