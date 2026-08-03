---
id: ISSUE-0002
title: "STOP does not consult INT64 on stock builds"
status: backlog
priority: medium
type: bug
owner: unassigned
created_at: 2026-08-03
updated_at: 2026-08-03
tags:
  - emu68
  - irq
  - jit
  - upstream
blockers:
related_files:
  - external/emu68/src/M68k_LINE4.c
  - docs/irq.md
---

# Summary

The m68k `STOP` instruction waits on the interrupt state (`INT64`) only on
PiStorm builds. On a stock build it masks `DAIF` and issues a bare `wfi()`,
never looking at `INT64` at all.

Upstream Emu68 defect, present at pin `9b4379a`. Not caused by anything in
`patches/emu68/`.

# Problem

`EMIT_STOP` (`M68k_LINE4.c`) is gated:

- `:1609` — `#ifndef PISTORM_ANY_MODEL`
- `:1611` — `EMIT(ctx, wfi())`

so the stock path is a bare wait-for-interrupt with no `INT64` test.

It happens to work, because `wfi` wakes on a masked IRQ. But the CPU resumes
without having consulted the interrupt state that the rest of the delivery
machinery is built on, so any interrupt source that sets `INT64` without also
raising a physical ARM IRQ at the right moment is not what wakes it.

This matters more here than the other two upstream findings: **`STOP` is where
an idle guest sits.** Exec's idle loop is a normal, continuous code path, not
an edge case, so anything wrong in how `STOP` interacts with interrupt delivery
is wrong most of the time the machine is idle.

# Goal

`STOP` on a stock build waits on the same interrupt state that delivers
interrupts, so waking and dispatching cannot disagree.

# What is left

Everything. Nothing has been changed, and the shape of the fix is not obvious:
the PiStorm branch waits on `INT64` because a second core publishes into it,
and a stock build has no equivalent publisher today. What the stock branch
should wait on depends on which delivery mechanism this project ends up using
(see ISSUE-0004 and `docs/irq.md`).

# Decisions taken

None. Deliberately parked: it is coupled to the delivery-mechanism question,
and fixing it before that is decided risks building on the wrong assumption.

# Acceptance criteria

- [ ] Behaviour of a guest executing `STOP` characterised on a stock build,
      with and without a pending host interrupt
- [ ] Decided what the stock branch waits on, consistent with the chosen
      delivery mechanism
- [ ] Idle guest wakes on the first interrupt, not the second or on a timeout

# Notes

Found by the AROS/m68k-emu68 port and re-verified here against pin `9b4379a`.

Worth connecting to the failure recorded in `docs/irq.md`: in the previous
incarnation of this project, a single-core build hung in Exec idle at
`pc=0xfc0f90` while the chipset kept generating VBL underneath. That hang was
diagnosed as a delivery-window problem rather than a `STOP` problem, but the
two are adjacent enough that this issue should be re-read when that ground is
covered again.

# Execution log

- 2026-08-03 — verified against pin `9b4379a`; issue opened, no work started.
