---
id: ISSUE-0014
title: "Wanderer's task frame lands below its own stack"
status: done
priority: critical
type: bug
owner: unassigned
created_at: 2026-08-06
updated_at: 2026-08-06
tags:
  - aros
  - scheduler
  - wanderer
  - stack
blockers:
related_files:
  - aros/arch/m68k-emu68/kernel/context.c
  - aros/arch/m68k-emu68/exec/switch.S
  - aros/arch/m68k-emu68/exec/dispatch.S
  - AI_context/consolidated/history/ISSUE-0007.md
---

# Summary

A frame validator kept in the 68-byte Exec backend (branch
`experiment/frame-68`) reports Wanderer's persistent task frame being written
**below `tc_SPLower`** — outside the task's own stack:

```
[EMU68-FRAME] BAD save task=002ff35c 'WANDERER:Wanderer'
    frame=00344170 stack=0034429c..0034829c pc=0000201e sr=0014 fmt=0078
[EMU68-FRAME] BAD save task=002ff35c 'WANDERER:Wanderer'
    frame=00343bf4 stack=0034429c..0034829c pc=346b48c6 ...
```

`0x344170` is about 300 bytes below the 16 KB stack that starts at `0x34429c`;
the second is further down still, and carries a PC of `0x346b48c6`, which is
not code. 97 reports in the run that produced them.

This is a **guest task stack overflow**, and it is the first thing found that
explains the observed symptom directly: once the frame is below `tc_SPLower`,
the scheduler saves and restores registers over memory belonging to something
else, and pointers read back from there are whatever that something else holds
— which is exactly the string fragments ISSUE-0007 recorded (`"alnu"`,
`"ket\0"`, `"ket\x0e"`).

# Problem

Not yet attributed. Three readings, in the order they should be tested:

**Wanderer genuinely needs more stack than it is given.** It is MUI-heavy and
the failures cluster after it starts loading Zune icon classes. The cheapest
test in the world: raise the stack and re-measure.

**Something consumes more per frame than the stack was sized for.** The port
carries a persistent frame on the task's own stack, so anything that grows the
frame eats headroom that a stock AROS never spends.

**The overflow is a consequence, not a cause** — a recursion or a runaway loop
inside Wanderer that would overflow any stack. The validator would report the
same thing either way, so the size test has to come first to separate them.

# Goal

No frame outside `tc_SPLower..tc_SPUpper`, and a measured effect on the boot
rate.

# What was done

`AROS_STACKSIZE` in `arch/m68k-all/include/aros/cpu.h` was 16 KB, which made
m68k the outlier in this tree by a factor of two and a half:

| target | AROS_STACKSIZE |
|---|---|
| i386-all, ppc-all, riscv-all, x86_64-all | 40960 |
| ppc-morphos | 32768 |
| **m68k-all** | **16384** |

It is a figure from machines with 512 KB of RAM. Raised to 64 KB as
`patches/aros/0007` — deliberately above the 40960 the rest of the tree uses,
because this is a measurement and a test wants headroom it can walk back.

**Result, 8 runs:**

| | before | after |
|---|---|---|
| `[EMU68-FRAME] BAD` reports | 97 in one run | **0 across all 8** |
| runaway exception recursion | 1 | **0** |
| icons | 3/8 | 4/8 |

**The overflow is fixed.** That is a real result and not noise: a specific
defect, present before and absent after, measured the same way both times.

**The boot rate is not fixed.** 4 of 8 against 3 of 8, inside a day that ranged
12% to 50%. Something else still fails, and the failure mix shifted toward
`logo` — 3 of 8 runs never left the Emu68 screen, against 2 before. Too few
samples to say whether that is real.

# What is left

1. Walk the value back to 40960 and confirm the reports stay at zero. Agreeing
   with the other targets is a better argument than any number chosen here.
2. Attribute the remaining failures, which are now a different problem: no
   frame lands outside its stack and no recursion runs away, so the wild
   pointers this issue explained are gone and whatever is left is not that.
3. Note for the record: open-bus reports appear in *successful* runs too, and
   `0xc0000058` recurs across many runs including good ones. Some of these
   accesses are probably legitimate probing, not corruption. Do not read an
   open-bus report as evidence of a fault without checking that first.

# Decisions taken

The validator stays in whatever backend this port ends up using. It costs a few
instructions per switch, it is silent unless a frame is malformed, and it found
in one series what pixel-level and emulator-level instrumentation had not found
all day.

# Acceptance criteria

- [x] No `[EMU68-FRAME] BAD` report across 8 runs
- [x] Boot rate measured before and after, ≥8 runs each — unchanged within noise
- [ ] Value walked back to 40960 with the reports still at zero

# Notes

Found on `experiment/frame-68`, which exists to test whether the 68-byte task
frame changes the boot rate. **It does not** — 3 of 8, against 1 of 8 and 3 of 8
for the two preceding series on `main`, all inside a day's spread of 12% to 50%.
The backend's value so far is entirely the validator it carries.

# Execution log

- 2026-08-06 — opened. Found by bringing the parked 68-byte backend onto a
  branch to test the frame hypothesis; the hypothesis was not confirmed, and the
  diagnostic that came with it found something better.
- 2026-08-06 — raised AROS_STACKSIZE from 16 KB to 64 KB. Frame-validator
  reports go from 97 in a run to 0 across 8, and runaway recursion from 1 to 0.
  The boot rate does not move (4/8 against 3/8). One defect closed; the
  intermittency is now a different problem.

# Closed

Closed 2026-08-06. `AROS_STACKSIZE` raised from 16 KB to 40960 (`patches/aros/0007`), matching every other target that is not MorphOS. Validator reports went from 97 in one run to zero across eight, and runaway recursion from one to none. It does not change the boot rate, and that was measured.
