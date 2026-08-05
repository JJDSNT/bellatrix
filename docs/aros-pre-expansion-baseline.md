# AROS Pre-Expansion Baseline

This baseline isolates the last known m68k-Emu68 state with SD-card boot but
without the port-specific Zorro expansion implementation. It exists to test
whether the intermittent startup and Wanderer failures predate the expansion
work.

## Historical Boundary

The relevant history is in `/home/jaime/AROS`, branch
`feature/m68k-emu68-baremetal`:

| Commit | Change |
|---|---|
| `645ff290f5` | Introduced the AROS SD-card stack. |
| `a63aaba6cb` | Unblocked the distribution build. This is the baseline. |
| `1b15598c21` | First port-specific Zorro change: added `arch/m68k-emu68/expansion`. |

Branch `feature/m68k-emu68-pre-expansion` points directly to `a63aaba6cb`.
It retains `expansion.library` in `CORERESIDENTS`, because DOS boot requires
the generic library, but contains no Emu68 Zorro backend, bus walk, DiagPoint,
or expansion ROM loader.

This distinction matters: removing `expansion.library` entirely produces an
`Exec Bootstrap Task` requester and cannot reach Wanderer. The historical
state used the generic non-Amiga stubs instead.

## Isolated Build

The branch is checked out separately so current source changes and generated
objects cannot enter the result:

```bash
cd /home/jaime/bellatrix
git -C /home/jaime/AROS worktree add \
  out/src/aros-pre-expansion feature/m68k-emu68-pre-expansion
mkdir -p out/build/aros-pre-expansion
cd out/build/aros-pre-expansion
../../src/aros-pre-expansion/configure --target=emu68-m68k
make kernel-link-emu68-m68k
```

The build must remain serial. This historical configuration selects a minimum
68000 CPU; the later 68040 target selection is deliberately not backported.
Downloaded source archives may be shared, but object files and the cross
toolchain must remain isolated.

Expected ELF:

```text
out/build/aros-pre-expansion/bin/emu68-m68k/AROS/aros-emu68-m68k.elf
```

## Test Procedure

Preserve the current ELF before installing the baseline. Boot with the same
Emu68 image, DTB, SD image, and `nocomposition` argument used by `run.sh`:

```bash
./run.sh --no-build --gui 2>&1 | tee /tmp/aros-pre-expansion.log
```

Run multiple trials and record whether Startup-Sequence completes, Wanderer
draws its volume icons, or Emu68 reports `CPU exception`, `Back from translated
code`, or a zero/invalid PC. A single successful boot is insufficient for this
intermittent failure.

## Interpretation

Failure on this baseline rules out the port-specific expansion implementation
as the root cause, though later changes may still alter timing. Stability here
and failure on the current port makes the commits after `a63aaba6cb` suitable
for a commit-by-commit bisection.
