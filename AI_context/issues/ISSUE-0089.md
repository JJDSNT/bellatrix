---
id: ISSUE-0089
title: "A task's own signal exception is not delivered at Permit(): upstream's signalexception test fails"
status: open
priority: medium
type: bug
owner: unassigned
created_at: 2026-10-25
updated_at: 2026-10-25
tags:
  - exec
  - scheduler
  - m68k
  - task-exception
blockers:
related_files:
  - aros/arch/m68k-emu68/exec/dispatch.S
  - aros/arch/m68k-emu68/exec/switch.S
  - aros/arch/m68k-emu68/kernel/schedule.c
  - aros/arch/m68k-emu68/kernel/context.c
  - tests/gl/task-exception
  - AI_context/issues/ISSUE-0088.md
---

# Summary

Upstream added `developer/debug/test/exec/signalexception` with `4ac3fbc085`
(M9 of [ISSUE-0088](ISSUE-0088.md)). On this port it fails every round:

```
SIGNAL EXCEPTION FAIL: dispatcher delivers the exception once
SIGNAL EXCEPTION FAIL: handler runs with interrupts enabled
SIGNAL EXCEPTION TEST: 6 failures
```

Its sibling `exceptioncontext` passes: 0 failures, 100 callbacks,
50,000,000 comparisons. Both run under `tests/gl/task-exception`, whose
header says how to put the two binaries on the card.

**Not a regression of the refresh.** It was tried twice. The first run was
the M9 build. The second was the same build with M9's two kernel changes
(`context.c` and `kernel_cpu.c`, which port `4ac3fbc085`) reverted, so the
ELF was relinked without them. Both produced the same six failures, so the
behaviour predates that port.

# What the failure says

The test sets an exception handler on its own task and signals itself
inside `Forbid()`. After `Permit()`, it spins up to 1,000,000 times waiting
for the handler. The failures fit an exception that is not delivered within
that spin:

- "delivers once" fails because `calls` never moves.
- "interrupts enabled" fails because `during` is never written.
- "nesting restored" passes, because nothing ran.

`exceptioncontext` gets its exceptions delivered, so delivery through the
dispatcher works here (`dispatch.S` → slow path → `emu68_DispatchFrame` →
`context.c`). What is missing is the *prompt* delivery that the generic path
expects:

1. `Signal()` on the running task with an exception pending sets `TF_EXCEPT`
   and calls `Reschedule()`, which, under `Forbid()`, only sets
   `SFF_AttnSwitch`.
2. `Permit()` sees `TDNestCnt < 0`, `IDNestCnt < 0` and the flag set, and
   calls `KrnSchedule()`.
3. `core_Schedule()` returns TRUE for a task with `TF_EXCEPT`, even alone, so
   the task should go through `Switch` and back through `Dispatch`, where the
   exception runs.

Something in 2 or 3 does not happen on this port. The candidates are:

- our `KrnSchedule()` (`kernel/schedule.c`, `Supervisor()` into exec's
  `Schedule`);
- our `switch.S`;
- the `IDNestCnt < 0` condition, which on this port is not tied to a
  physical interrupt gate (CLAUDE.md, *IRQ bridge*);
- the signal fast path in `signal_fast.S`, whose Paula write that raises
  the software interrupt is compiled out for `__EMU68__` (`0002`).

The last one is the first to check: with no software interrupt, a switch
deferred by `Reschedule()` waits for the next hardware interrupt, the 50 Hz
heartbeat. A million JITted spins can finish before it.

# Why it matters

Programs that use task exceptions (signal handlers in `ixemul`, BCPL break
handling, some network code) expect the handler to run before `Permit()`
returns control. Here it runs on the next tick instead. That is late, not
lost, but it is not what Kickstart does.
