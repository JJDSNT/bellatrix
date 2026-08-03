---
id: ISSUE-0003
title: "The core-0 branch of IRQHandler() is unreachable"
status: backlog
priority: low
type: bug
owner: unassigned
created_at: 2026-08-03
updated_at: 2026-08-03
tags:
  - emu68
  - irq
  - dead-code
  - upstream
blockers:
related_files:
  - external/emu68/src/aarch64/vectors.c
  - docs/irq.md
---

# Summary

`IRQHandler()` contains a `cpu_id == 0` branch that acknowledges the GIC and
reports an interrupt with no `INTENA` gate. It cannot execute.

Upstream Emu68, present at pin `9b4379a`. Harmless today; recorded because it
reads like a second, ungated delivery path.

# Problem

`IRQHandler()` has exactly one caller, `IRQonOtherCores`, which is reached only
after the vector table has already tested `MPIDR_EL1 & 3` and branched away for
core 0 — that is, only for cores 1 to 3. Core 0 is handled inline in
`curr_el_spx_irq` and `eret`s; the SP0 vectors go to `SYSHandler`.

So the `cpu_id == 0` arm of `IRQHandler()` is dead code.

The reason to care is not the dead code itself. It is that anyone reading
`vectors.c` looking for "how does a host interrupt reach the guest" finds two
answers, one of which gates on `INTENA` and one of which does not, with no
indication that the second never runs. It would become a real inconsistency the
moment anything routes a core-0 interrupt through `IRQHandler()`.

# Goal

One readable answer to "how does a core-0 interrupt reach the guest".

# What is left

Everything. Two candidate resolutions:

1. Delete the branch — it is unreachable, and deleting it makes the single
   real path obvious.
2. Keep it and gate it identically to the fast path, so that routing a core-0
   interrupt through it later is safe by construction.

The choice depends on whether anything is ever expected to route core-0
interrupts through the C handler, which in turn depends on the delivery
mechanism question (ISSUE-0004).

# Decisions taken

None.

# Acceptance criteria

- [ ] Confirmed there is no build configuration in which the branch executes
- [ ] Branch either removed or gated consistently with the fast path
- [ ] `docs/irq.md` updated if the delivery picture changes

# Notes

Found by the AROS/m68k-emu68 port and re-verified here against pin `9b4379a`.

Lowest priority of the three upstream findings: no behaviour depends on it. It
is a readability and future-safety issue.

# Execution log

- 2026-08-03 — verified against pin `9b4379a`; issue opened, no work started.
