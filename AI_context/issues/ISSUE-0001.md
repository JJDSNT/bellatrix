---
id: ISSUE-0001
title: "JITCTRL2 bit 29 always reads 0 for fast-path interrupts"
status: review
priority: low
type: bug
owner: unassigned
created_at: 2026-08-03
updated_at: 2026-08-17
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

**One thing: the observation.** The fix is in and the consumers were audited,
but nobody has read the register back.

That gap is easy to mistake for closed, so it is worth being explicit: the
acknowledge in `Platform_Autovector()` reads JITCTRL2, ORs bit 29 and writes
it back, and it sets the bit **unconditionally**. It would behave identically
if the read returned 0. So the acknowledge working is not evidence that the
read reflects pending state, which is exactly what this issue is about.

One boot that reads the register inside the level-6 handler and reports the
bit closes it.

# Decisions taken

**2026-08-17 — fixed locally, as part of `patches/emu68/0010`.** Not as a
courtesy: moving platform interrupts onto `INTF.ARM` (ISSUE-0039) makes the
guest acknowledge through JITCTRL2 bit 29, so a byte that reads back as
"nothing pending" is not a cosmetic defect there — it is the difference
between the channel working and not. The two fast paths now store `1`, which
is what `M68kReportInterrupt()` already stored (`ExecutionLoop.c:595`).

Upstream vs local: local, because it had to ship with the change that needs
it. It remains a small self-contained fix and a good candidate to send
upstream separately.

# Acceptance criteria

- [x] Every consumer of `INTF.ARM` audited; confirmed the byte is never read as
      a level
- [x] Fast path and `M68kReportInterrupt()` agree on the stored value — both
      store `1` as of `patches/emu68/0010`
- [ ] A guest reading `JITCTRL2` after a fast-path interrupt sees bit 29 set
- [x] Decision recorded on upstream vs local patch

# Notes

Found by the AROS/m68k-emu68 port and re-verified here against pin `9b4379a`.
See `docs/irq.md`, "Known defects in the surrounding Emu68 code".

# Execution log

- 2026-08-03 — verified against pin `9b4379a`; issue opened, no work started.
- 2026-08-17 — Fixed by `patches/emu68/0010` as a precondition of ISSUE-0039,
  not on its own merits. Status `review`: three of the four acceptance
  criteria are met and the fourth is an observation nobody has made yet.
