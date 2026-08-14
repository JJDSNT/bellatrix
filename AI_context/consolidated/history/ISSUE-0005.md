---
id: ISSUE-0005
title: "Access-size handling gaps in our interrupt-register intercepts"
status: done
priority: high
type: bug
owner: unassigned
created_at: 2026-08-03
updated_at: 2026-08-14
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
data — was found and fixed on 2026-08-03. See
`AI_context/consolidated/history/ISSUE-0006.md`, which also records the general
rule the fix produced. These two were left.

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

Assessed against ISSUE-0007 on 2026-08-05 and left at `medium`. The intercepts
are on the failing path — the `INTENA` shadow is what gates level-6 delivery —
but neither open item can produce the observed failure. `Platform_Init()` and
`Platform_Autovector()` access `INTENA`/`INTREQ`/`INTENAR` as words only, so the
ungated write side and the unhandled 32-bit span across `0xdff01c` are both
unreachable from our own code. The serial log confirms it from the other end:
level-6 entries keep arriving after the stall, so the shadow is not the thing
that stopped. Fix these when the patch is next regenerated — which ISSUE-0008
requires anyway — rather than as part of chasing the desktop.

# Execution log

- 2026-08-03 — the read-side defect found by building the series clean and
  fixed the same day; these two identified alongside it and left open.
- 2026-08-05 — reviewed for relevance to ISSUE-0007 and kept at `medium`: both
  open items are unreachable from the port's own word-sized accesses. Coupled to
  ISSUE-0008, which has to regenerate `patches/emu68/0001` regardless.

# Update 2026-08-06: this is the shape of the boot failure's suspect

ISSUE-0007 found that the dominant boot failure is Emu68's ARM stack collapsing
from ~700 bytes of normal use to fully exhausted between two five-second
samples — unbounded recursion through `curr_el_spx_sync`, the synchronous
exception vector every intercepted access goes through. In two of the first
three captured stalls the guest address in flight was `0xdff01e`, INTREQR.

The second gap listed above is the right shape for that: a 32-bit access
spanning `INTENAR`+`INTREQR` (`0xdff01c`–`0xdff01f`) matches neither intercept,
because both require `size <= 2`, and falls through. **Whether that fall-through
can itself fault is the question** — if it can, the handler re-enters itself and
the stack goes in milliseconds.

Two things keep this honest. Nothing has proven the fall-through faults; the
note above concludes it reads RAM, which would be wrong-but-harmless. And the
port is moving to IPL injection (ISSUE-0010), which removes these intercepts
entirely and would make the question moot rather than answered. Worth resolving
anyway: if a fall-through inside the fault handler can fault, that is a defect
independent of which registers are intercepted.

# Closed 2026-08-14 — the patch this describes no longer exists

Both open items are defects *inside* `patches/emu68/0001-emulate-amiga-interrupt-registers.patch`.
That patch was removed on 2026-08-13 (`11be65d`, "refactor(emu68): drop the
emulated interrupt registers, renumber the series") once the bus observer
established that the guest never touches `$DFF000` at all: host interrupts
arrive as an IPL level (`patches/emu68/0002`), so there is no `INTENA`/`INTREQ`
shadow left to intercept at any access size.

Closed as obsolete rather than fixed. Nothing was decided about the two
questions and nothing needs to be — the code they were about is gone. If an
interrupt-register intercept is ever reintroduced, the rule this issue was
written to enforce is the part worth carrying forward, and it lives in
`docs/irq.md`: **claiming an access is a promise to satisfy it completely for
the size requested.** If an intercept only makes sense at one or two widths,
that belongs in the condition, not in a comment.

The 2026-08-06 update above speculated that a 32-bit access spanning
`INTENAR`+`INTREQR` might be re-entering the fault handler and exhausting the
ARM stack. That was never confirmed, and the boot failure it was speculating
about turned out to be something else entirely — an undersized TLSF split in
`rom/kernel/tlsf.c`, see `AI_context/consolidated/history/ISSUE-0007.md`. The
speculation is left in place above rather than edited out, because a plausible
mechanism that was wrong is worth being able to recognise again.
