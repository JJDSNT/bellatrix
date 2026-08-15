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

# What is left

- `scripts/release.sh`: refuse unless `setup.sh --verify` reports `applied`, run
  `--pack`, stamp the version, name and checksum the asset, publish with
  `gh release create/upload --clobber`.
- Stamp `version.txt` inside the archive — tag, commit, submodule pins.
- Adopt `bellatrix-<tag>-pi3-system.tar.xz`, leaving `-boot` free for the split
  design.
- Measure one cold `build-aros.sh full` — that number decides between hosted CI
  and a self-hosted runner.

# Decisions taken

- Start from a workstation script (design A in `docs/release.md`), not from CI.
  The script is what a workflow would call anyway, so it is not throwaway work.
- Keep `xz`. It is 19.2 MB against 28.0 MB for `tar.bz2`, and matching the AROS
  convention buys nothing else.
- Keep the archive's `./…` paths with no top-level directory. Identity goes in
  the filename and in a stamped file, never in a wrapping directory.

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
