---
id: ISSUE-0006
title: "INTENAR/INTREQR reads returned uninitialised *value2 on wide accesses"
status: consolidated
priority: high
type: bug
owner: agent
created_at: 2026-08-03
updated_at: 2026-08-03
tags:
  - emu68
  - irq
  - patches
  - ours
blockers:
related_files:
  - patches/emu68/0001-emulate-amiga-interrupt-registers.patch
  - external/emu68/src/aarch64/vectors.c
  - docs/irq.md
  - docs/emu68.md
---

# Summary

The `INTENAR`/`INTREQR` read intercepts added by `patches/emu68/0001` claimed
accesses of *any* size while filling only `*value`. A 16-byte read of those
addresses returned "handled" with `*value2` untouched, handing the guest a
register full of stack garbage.

Found and fixed on 2026-08-03. Recorded here because the general rule it
produced outlives the fix.

# Problem

`SYSReadValFromAddr(value, value2, size, far)` must fill `*value2` as well as
`*value` for `size == 16`. Upstream's implementation ends in a fall-through
`switch(size)` whose `case 16` writes both, so upstream gets this for free. An
intercept that returns early does not.

The intercepts as first written were:

```c
if ((far & ~1) == INTENAR) {        /* no size condition */
    ...
    *value = ...;                   /* *value2 never touched */
    return 1;
}
```

The autoconfig read immediately below them already gated correctly on
`size == 1`; these two did not.

Reachability is not theoretical: the JIT emits `LDP` for guest memory access,
and `SYSPageFaultReadHandler` calls `SYSReadValFromAddr(..., 16, ptr)` for it.
An m68k access compiled to `LDP` against `0xdff01c` takes the path.

# Goal

Every intercept satisfies the access it claims, for the size requested.

# What was done

Both intercepts gated on `size <= 2` — byte and word, which is all a 16-bit
chip register can be read with. Wider accesses fall through to the memory path,
reproducing pre-series behaviour exactly.

The fix was made at the source, in commit 1 of the series, and the series
regenerated through the workflow in `docs/emu68.md` (`git am`, amend, replay
with `cherry-pick`, `format-patch --zero-commit --no-signature`).

# What is left

Nothing for this defect. Two adjacent gaps of the same family were identified
at the same time and left open as **ISSUE-0005**: the write side has no size
gate, and a 32-bit access spanning `INTENAR`+`INTREQR` falls through to RAM.

# Decisions taken

- Gated on `size <= 2` rather than `size == 1 || size == 2`. Both express the
  same intent; the former was chosen because 4/8/16 falling through to memory
  reproduces pre-series behaviour exactly, which was the conservative choice
  while fixing a live defect.
- Fixed at the source rather than by re-adding `-Wno-error=maybe-uninitialized`.

# Acceptance criteria

- [x] `INTENAR`/`INTREQR` no longer claim accesses they cannot satisfy
- [x] Wider accesses fall through unchanged
- [x] Series builds clean with `-Werror` (GCC 13)
- [x] Series still reproduces a single known tree (`b9e1d18`)
- [x] `./run.sh` boots to `[BOOT] Booting Emu68 runtime/AArch64 BigEndian`
- [x] Documented in `docs/irq.md` and `docs/emu68.md`

# Notes

**Why it survived so long.** The defect was present from the first version of
the series and did not surface, because the build that carried it passed
`-Wno-error=maybe-uninitialized` — set in an ad-hoc toolchain file from an
earlier session. GCC 13 had been reporting it all along, demoted to a warning
in a wall of build output. It was found by building the series clean for the
first time, as part of standing up `scripts/build.sh`.

Two things worth keeping from that:

- A warning suppressed at the command line is invisible in the source. The
  suppression lived in a scratch toolchain file that no longer exists, so
  nothing in the tree recorded that a diagnostic had been turned off.
- The first real build of a patch series is a review step, not a formality.

**The rule it produced**, now in `docs/irq.md`: claiming an access is a promise
to satisfy it completely for the size requested. If an intercept only makes
sense at one or two widths, that belongs in the condition, not in a comment.

# Execution log

- 2026-08-03 — first clean build of the series fails with
  `-Werror=maybe-uninitialized` at the use site in `SYSPageFaultReadHandler`.
- 2026-08-03 — pristine upstream at pin `9b4379a` built with the same compiler:
  clean. Defect isolated to the series.
- 2026-08-03 — fixed in commit 1, series regenerated, tree `e17d76c` →
  `b9e1d18`, build and QEMU boot verified. Commit `e466c59`.
