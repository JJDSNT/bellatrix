---
id: ISSUE-0001
title: "JITCTRL2 bit 29 always reads 0 for fast-path interrupts"
status: backlog
priority: low
type: bug
owner: unassigned
created_at: 2026-08-03
updated_at: 2026-08-03
tags:
  - emu68
  - irq
  - upstream
blockers:
related_files:
  - external/emu68/src/M68k_LINE4.c
  - external/emu68/src/aarch64/vectors.c
  - external/emu68/src/ExecutionLoop.c
  - docs/irq.md
---

# Summary

A guest reading `JITCTRL2` (`MOVEC` `0x1e0`) to ask "is a host interrupt
pending?" gets 0 for every interrupt delivered through the core-0 assembly fast
path — which is essentially all of them.

Upstream Emu68 defect, present at pin `9b4379a`. Not caused by anything in
`patches/emu68/`.

# Problem

The `JITCTRL2` read path builds the register from `INTF` bytes
(`M68k_LINE4.c:2365`):

```c
ldrb_offset(ctxreg, tmp, __builtin_offsetof(struct M68KState, INTF.ARM)),
bfi(reg, tmp, 29, 1)
```

`bfi(..., 29, 1)` is a one-bit field insert, so only bit 0 of `INTF.ARM`
reaches bit 29 of the result.

The two writers disagree about what goes in that byte:

| Writer | Value | Where |
|---|---|---|
| core-0 IRQ fast path | `6` | `vectors.c:165` |
| core-0 FIQ fast path | `6` | `vectors.c:190` |
| `M68kReportInterrupt()` | `1` | `ExecutionLoop.c:558` |

`6 & 1 == 0`. Interrupts reported by `M68kReportInterrupt()` read back
correctly; interrupts delivered by the fast path read back as "nothing
pending".

Delivery itself is unaffected — the dispatch path tests the byte for
truthiness, not for bit 0 — so this is invisible unless a guest queries the
register.

# Goal

`JITCTRL2` bit 29 reflects host-interrupt-pending regardless of which path
delivered the interrupt.

# What is left

Everything. Nothing has been changed.

The obvious fix is to store `1` instead of `6` in the two fast-path handlers:
the value is only ever tested for truthiness, never used as a level (the level
is a constant 6 by contract, decided in `ExecutionLoop.c`'s arbitration). That
should be confirmed by reading every consumer of `INTF.ARM` before changing it.

# Decisions taken

None yet. Open question: fix locally as a patch in `patches/emu68/`, or report
upstream and wait. It is a small, self-contained fix with no dependency on
anything else here, which makes it a good candidate to send upstream first.

# Acceptance criteria

- [ ] Every consumer of `INTF.ARM` audited; confirmed the byte is never read as
      a level
- [ ] Fast path and `M68kReportInterrupt()` agree on the stored value
- [ ] A guest reading `JITCTRL2` after a fast-path interrupt sees bit 29 set
- [ ] Decision recorded on upstream vs local patch

# Notes

Found by the AROS/m68k-emu68 port and re-verified here against pin `9b4379a`.
See `docs/irq.md`, "Known defects in the surrounding Emu68 code".

# Execution log

- 2026-08-03 — verified against pin `9b4379a`; issue opened, no work started.
