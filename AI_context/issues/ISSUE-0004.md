---
id: ISSUE-0004
title: "INTF.IPL is inert on stock builds — the IPL delivery mechanism is unreachable"
status: backlog
priority: medium
type: bug
owner: unassigned
created_at: 2026-08-03
updated_at: 2026-08-03
tags:
  - emu68
  - irq
  - ipl
  - upstream
  - architecture
blockers:
related_files:
  - external/emu68/src/ExecutionLoop.c
  - external/emu68/src/pistorm/ps_protocol.c
  - docs/irq.md
---

# Summary

`INTF.IPL` is how an external interrupt controller hands the CPU a level — the
mechanism PiStorm itself uses. On a stock build, writing it has no effect: the
arbitration accepts the field only through a code path whose level source is a
`return 0` stub.

Upstream Emu68, present at pin `9b4379a`.

# Problem

`ExecutionLoop.c:325` arbitration:

```c
#if defined(PISTORM)                 /* PiStorm32: use INTF.IPL directly */
    if (ctx->INTF.IPL > level) level = ctx->INTF.IPL;
#else                                /* a stock build lands here */
    if (ctx->INTF.IPL) {
        ipl_level = GetIPLLevel();   /* → return 0 on non-PISTORM_CLASSIC */
        if (ipl_level > level) level = ipl_level;
    }
#endif
```

`GetIPLLevel()` is a `return 0` stub outside `PISTORM_CLASSIC`
(`ExecutionLoop.c:296`). So on a stock build `INTF.IPL` opens the `if` and then
the level is discarded.

This is the same shape of trap as the one the patch series in `patches/emu68/`
exists to fix: the field is declared, the code compiles, the writers exist for
PiStorm — and no stock build can use it.

# Goal

Decide whether this project wants the IPL mechanism at all, and if so make it
reachable.

# What is left

The code change is small — roughly thirteen lines, letting a non-PiStorm build
take the direct `INTF.IPL > level` branch. The previous incarnation of this
project did exactly that.

**What is not small is the question behind it.** `INTF.IPL` carries a level,
not a register, so whoever writes it must already have made the INTENA/INTREQ
decision. Adopting it means something other than Emu68 owns the interrupt
state, and that is one of three mutually exclusive designs laid out in
`docs/irq.md`. There is no chipset here yet, so there is nothing to own it, and
the decision cannot be taken.

**Prior art, and a warning.** The previous incarnation patched the arbitration
and it did not settle the problem: single-core Emu68 still hung in Exec idle at
`pc=0xfc0f90` polling INTREQ, across 400 chipset frames and ~125M instructions
with `ipl=0` and `int32=0` throughout, while VBL was being generated
underneath. Multicore delivered correctly over the same interface. The failure
was in *when* the level was published relative to the JIT's execution window,
not in the interface. Anyone picking this up should treat "make `INTF.IPL`
reachable" and "make delivery reliable" as two separate problems.

# Decisions taken

Deferred, deliberately. Recorded in `docs/irq.md`: the choice between the three
delivery mechanisms turns on who owns INTENA/INTREQ, and cannot be made before
there is a chipset.

# Acceptance criteria

Only meaningful once the delivery mechanism is chosen. If IPL is chosen:

- [ ] A non-PiStorm build takes the direct `INTF.IPL` branch
- [ ] A written level reaches the guest as an m68k interrupt, honouring the SR
      IPL mask
- [ ] Delivery verified under whatever core topology is in use, not just one
- [ ] The publish-vs-quantum question answered explicitly, not by observation
      that it happens to work

# Notes

Not a defect that hurts anything today — nothing writes `INTF.IPL` here. It is
filed because the mechanism looks available from the outside and is not, which
is exactly the kind of thing that costs a day when it is discovered mid-task.

# Execution log

- 2026-08-03 — verified against pin `9b4379a`; issue opened, no work started.
