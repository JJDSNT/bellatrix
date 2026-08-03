---
id: ISSUE-0005
title: "Access-size handling gaps in our interrupt-register intercepts"
status: backlog
priority: medium
type: bug
owner: unassigned
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
---

# Summary

Two remaining places where the intercepts added by `patches/emu68/0001` do not
handle access size correctly. Both are in code this project owns, not upstream.

A third defect of the same family — `INTENAR`/`INTREQR` reads claiming any size
while filling only `*value`, so a 16-byte read handed the guest uninitialised
data — was found and fixed on 2026-08-03. These two were left.

# Problem

## 1. The write side has no size gate

`SYSWriteValToAddr()` intercepts `INTENA` (`vectors.c:756`) and `INTREQ`
(`:771`) at any access size. There is no uninitialised-data hazard here, since
the values are inputs rather than outputs, but a wide write to those addresses
is currently swallowed as though it were a word write.

The read side now gates on `size <= 2`; the write side is asymmetric with it
for no stated reason.

## 2. A 32-bit access spanning INTENAR+INTREQR falls through to RAM

`0xdff01c`–`0xdff01f` is two 16-bit registers. A single 32-bit access covers
both. After the `size <= 2` gate, such an access falls through to the memory
path and reads RAM instead of the shadow.

This is not a regression — it did the same before the series — but it is wrong,
and it is now wrong *next to* code that gets it right, which makes it look
deliberate.

# Goal

Every access to the emulated interrupt registers is either handled correctly
for its size, or falls through for a stated reason.

# What is left

Both. Neither has been changed.

For (1): decide whether the write side should mirror the read gate, or whether
a wide write to a chip register is worth handling. Note this changes the
behaviour of a patch that is otherwise verified, so it needs its own reasoning
rather than being done for symmetry alone.

For (2): decide whether a 32-bit access spanning the pair is worth supporting.
On real hardware a longword read of `0xdff01c` reads INTENAR then INTREQR. If
supported, it must fill `*value` from both halves; if not, the fall-through
should be commented as intentional.

# Decisions taken

The read-side gate landed as `size <= 2` rather than `size == 1 || size == 2`,
because 4/8/16 falling through to memory reproduces pre-series behaviour
exactly, which was the conservative choice while fixing a live defect.

# Acceptance criteria

- [ ] Write side either gated or documented as deliberately ungated
- [ ] 32-bit span either handled or documented as deliberately unhandled
- [ ] `patches/emu68/0001` regenerated through the workflow in `docs/emu68.md`
- [ ] `./scripts/setup.sh --reset && ./scripts/build.sh` clean
- [ ] Series tree hash in `docs/emu68.md` updated

# Notes

The general rule these come from, recorded in `docs/irq.md`: claiming an access
is a promise to satisfy it completely for the size requested. If an intercept
only makes sense at one or two widths, that belongs in the condition, not in a
comment.

Worth doing together — same file, same family, one regeneration of the patch.

# Execution log

- 2026-08-03 — the read-side defect found by building the series clean and
  fixed the same day; these two identified alongside it and left open.
