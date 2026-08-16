---
id: ISSUE-0026
title: "The prebuilt toolchain can only be installed into this port's own build tree"
status: backlog
priority: medium
type: feature
owner: unassigned
created_at: 2026-08-16
updated_at: 2026-08-16
tags:
  - build
  - toolchain
  - ci
  - aros
blockers:
related_files:
  - scripts/build-aros.sh
  - .github/workflows/toolchain.yml
  - docs/aros.md
  - AI_context/issues/ISSUE-0024.md
---

# Summary

The prebuilt toolchain published by CI already covers every m68k AROS target,
but nothing can install it anywhere except this port's own build tree. A second
target — `amiga-m68k`, for testing an upstream change against the machine it
runs on — either compiles gcc again or gets the tarball unpacked by hand.

# Problem

`build-aros.sh` has the whole mechanism and it works: local cache, then the
`toolchain-<digest>` GitHub release, then and only then an offer to compile
(refused outright when there is no terminal to ask). What it does not have is a
way to aim that at a tree other than `out/build/aros` for a target other than
`$TARGET`; both are fixed near the top of the script.

**The cached artifact is already correct for the other target.** The key is
scoped by CPU, not by AROS architecture:

```
toolchain_key() { echo "$(toolchain_digest)-${TARGET##*-}-$(host_arch)-glibc$(host_glibc)"; }
```

`${TARGET##*-}` turns `emu68-m68k` into `m68k`, so the published
`a88db85e62ede04f-m68k-linux-x86_64-glibc2.39.tar.xz` is as much `amiga-m68k`'s
compiler as it is ours. That is by design rather than luck — AROS gates its
crosstools stages per CPU inside one directory
(`.installflag-gcc-<version>-m68k`), and the comment above `toolchain_key()`
says the CPU is in the key precisely so two AROS targets do not overwrite each
other's cache entry.

So the gap is only the plumbing.

# Goal

Installing the prebuilt toolchain into any already-configured AROS build tree is
one command, and uses the same lookup order — local cache, then release.

# What is left

1. **Let the destination and target be arguments.** `BUILD` and `TARGET` are
   read from the environment or flags rather than fixed, so the existing
   `restore_toolchain()` can serve another tree unchanged. Everything below it —
   `pick_compatible()`, the glibc-compatibility rule, the checksum check — is
   already target-agnostic.
2. **A `--install-toolchain <dir>` mode.** It does exactly three things: work
   out the file name from `toolchain_key()`, find it (local cache, else download
   the release asset and check its `.sha256`), and extract `crosstools/` into
   `<dir>/bin/<host>/tools/`.

   It deliberately does **not** run `configure`: the wrappers that sit beside
   `crosstools/` — `<cpu>-<arch>-elf-gcc`, `aros-ld` — are `configure`'s output
   and hardcode that tree's absolute path, which is the same reason only
   `crosstools/` is ever packed. So the order of use is `configure` first, this
   second, filling the hole `configure` leaves.

   It also does **not** fall back to compiling. If nothing compatible is on
   offer it says so and stops, rather than starting three hours of gcc in a tree
   that is not ours.
3. **Say the ARG_MAX thing where someone will read it.** Linking
   `stdc.library` passes every object on one command line; a long build-tree
   prefix times hundreds of paths overflows it and the error —
   `/bin/sh: Argument list too long` — names nothing useful. Refusing a build
   directory beyond some length, or just warning, would save the next person the
   hour it cost. Recorded in `docs/aros.md` for now.

# Decisions taken

**The tarball stays CPU-keyed.** Adding the AROS architecture to the key would
publish a second identical artifact for every target, and the whole point of the
digest is that it moves rarely.

**Only `crosstools/` is ever packed**, unchanged from today. A tree that lost its
wrappers is sent back through `configure`, not patched up from a tarball.

# Acceptance criteria

- [ ] A second AROS target can be given the prebuilt toolchain without a manual
      `tar`
- [ ] It uses the same lookup order, including the release fetch and the
      checksum check
- [ ] `build-aros.sh` for this port behaves exactly as before
- [ ] The ARG_MAX trap is either guarded or documented where a builder looks

# Notes

**The manual recipe, until this exists** (`docs/aros.md` has it with the
reasoning):

```bash
cd /somewhere/short && /path/to/external/aros/configure --target=amiga-m68k
tar xJf ~/.cache/bellatrix/toolchain/$(cd /path/to/bellatrix && \
    ./scripts/build-aros.sh --toolchain-key).tar.xz \
    -C /somewhere/short/bin/linux-x86_64/tools/
```

`--toolchain-key` exists for CI and prints exactly the file name to look for,
which is what makes this two lines instead of guessing.

**Why a second target is worth building at all.** `ISSUE-0024` proposes changes
to code that only runs on `m68k-amiga`. Compiling them there is the difference
between "this builds" and "we cannot test this", and one object file compiled
for that target was already enough to overturn a wrong conclusion once — see the
retraction in that issue's log. The full ROM is a further step and currently
blocked; that blocker is recorded there, not here.

# Execution log

- 2026-08-16 — Opened, after installing the toolchain into an `amiga-m68k` tree
  by hand while chasing `ISSUE-0024` — which worked, but skipped the release
  lookup and verified no checksum, which is most of what the mode is for. Confirmed the published release
  `toolchain-a88db85e62ede04f` carries
  `a88db85e62ede04f-m68k-linux-x86_64-glibc2.39.tar.xz`, that
  `./scripts/build-aros.sh --toolchain-key` prints exactly that name, and that
  extracting it into a freshly configured `amiga-m68k` tree gives a working
  `m68k-aros-gcc (GCC) 6.5.0` with no compilation at all.
