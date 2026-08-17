---
id: ISSUE-0039
title: "Platform interrupts move to INTF.ARM with a guest-owned acknowledge, leaving INTF.IPL to the Amiga domain"
status: done
priority: high
type: refactor
owner: agent
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - emu68
  - irq
  - architecture
  - rigel
blockers:
related_files:
  - patches/emu68/0010-assert-host-interrupts-on-intf-arm-with-a-guest-owned-ack.patch
  - patches/emu68/0001-make-ipl-injection-reachable-on-stock-builds.patch
  - patches/emu68/0002-deliver-host-interrupts-as-an-ipl-not-through-a-shadow.patch
  - aros/arch/m68k-emu68/platform/platform.c
  - external/emu68/src/ExecutionLoop.c
  - external/emu68/src/M68k_LINE4.c
  - docs/New_emu68.md
  - docs/Rigel_integration.md
  - AI_context/issues/ISSUE-0001.md
---

# Resolved (2026-08-17)

Platform interrupts are delivered on `INTF.ARM` and acknowledged by the guest
through `MOVEC` on JITCTRL2 bit 29. Commit `4c800fa`. Verified by the user on
two independent runs, both reaching the icons, plus four here.

The three items under "What is left" are not this issue's work and each has a
home:

- **Confirm bit 29 reads back set** is `ISSUE-0001`'s last acceptance
  criterion, and that issue is open in `review` for exactly it.
- **Push versus pull for Rigel** is a decision to take when Rigel exists. The
  material for it -- that `docs/New_emu68.md` section 15 and
  `docs/Rigel_integration.md` sections 25-26 disagree, and that section 28
  already forbids the auto-clear -- is recorded below and in the message of
  `patches/emu68/0010`.
- **Watch this under a second consumer** happened faster than expected:
  `dma.resource` arrived the same day and registers a per-channel handler.
  What that costs the system is `ISSUE-0042`.

# Summary

This port delivered every physical interrupt through `INTF.IPL`. That was
always transitional — `docs/New_emu68.md` section 3 says so in as many words —
and it put platform interrupts on the channel that belongs to the Amiga
chipset domain. They now go through `INTF.ARM`, acknowledged by the guest,
with `INTF.IPL` left for Rigel.

The question that opened this was whether Emu68 even *has* an `INTF.ARM` →
IPL path or only the IPL one, because interrupts are where disk, USB and
Bluetooth all meet. It has both, and the ARM one was never the problem.

# What was actually in the way, and what was not

The **consumption** of `INTF.ARM` is unconditional and predates any chipset
consideration (`ExecutionLoop.c:341-346`):

```c
if (ctx->INTF.ARM)      level = 6;
else if (ctx->INTF.PPC) level = 2;
...
if (ctx->INTF.IPL > level) level = ctx->INTF.IPL;
```

What did not fit was upstream's **lifecycle**, which is coupled to Paula at
both ends: armed only when the guest has `INTEN|EXTER` set in the INTENA
shadow, cleared only by a guest write to INTREQ — one page fault on `$DFF000`
per arm and per acknowledge. `patches/emu68/0002` removed that in the only way
available at the time, by moving to `INTF.IPL`. The reasoning was about the
lifecycle and was read afterwards as being about the channel.

# The acknowledge already existed

`docs/New_emu68.md` section 18 asks for a `M68kClearHostInterrupt()`. Stock
Emu68 ships it, as `MOVEC` on the JITCTRL2 control register (`0x1e0`):

| direction | emitter | behaviour |
|---|---|---|
| write | `M68k_LINE4.c:2100-2104` | `tbz(reg, 29, 2)` → `strb WZR` into `INTF.ARM`; **write 1 to clear**. Bit 30 does the same for `INTF.PPC`. |
| read | `M68k_LINE4.c:2363-2365` | `INTF.ARM` comes back in bit 29 |

Register to register. No MMIO, no fault, no register emulation. Nothing had to
be added to Emu68 — the mechanism was unused only because nothing wrote the
byte.

# The change

**`patches/emu68/0010`** — the core-0 IRQ and FIQ fast paths store into
`INTF.ARM` instead of `INTF.IPL`, still with no INTENA test and no shadow.

**`aros/.../platform/platform.c`** — `platform_host_irq_ack()` does the
deassert: read JITCTRL2, set bit 29, write back.

Two decisions worth keeping, because neither is obvious:

1. **The value stored is `1`, not `6`.** The arbitration only tests the byte
   for truthiness — the level is a constant 6 by contract — but JITCTRL2
   exposes *bit 0* of it, so storing 6 makes the register read back "nothing
   pending" (`6 & 1 == 0`). That is `ISSUE-0001`, and fixing it was a
   precondition for the acknowledge rather than a bonus. It also makes the two
   writers agree: `M68kReportInterrupt()` already stored 1
   (`ExecutionLoop.c:595`).
2. **The acknowledge runs before `Dispatch()`, not after.** ARM interrupts are
   masked for the whole handler — Emu68 sets the I bit on the `eret` and the
   JIT only reopens the gate when the guest's SR mask drops below 6 — so a
   source asserting while we are inside cannot re-latch `INTF.ARM`, and
   clearing afterwards would discard it. Clearing first makes the worst case
   one spurious level 6 with nothing to do, instead of one lost.

**Both halves must land together.** `INTF.ARM` is a latch: an unacknowledged
one re-enters level 6 the moment the guest's RTE drops the SR mask.

# Evidence

Six boots reached the Wanderer desktop after the change — one by hand with a
screendump of the themed desktop, two run by the user on their own, and three
earlier ones establishing the pre-change baseline for comparison. One timed run
stalled after the desktop drew, with the PC pinned at `fffffff0005a2ad0` for
245 s.

**That stall is not evidence of a regression and was briefly treated as if it
were.** It is one occurrence, at the same point in the boot and at the same
rate as `ISSUE-0037`, which predates this change. It is recorded under
`out/boot-timing/2026-08-17T172633Z/` and belongs to that issue's bucket.

The delivery argument is closed by elimination rather than by the boots alone:
after `0010` **nothing writes `INTF.IPL`**, and `INT_shadow.ARMPending` has had
no writer since `0002`. If `INTF.ARM` were not being asserted, `INT64` would
always be zero, level 6 would never be taken, and the boot would stall in the
SD driver before DOS. If the acknowledge were not working, the latch would
re-enter level 6 after every RTE and nothing would progress.

# What is not proven

`ISSUE-0001`'s remaining acceptance criterion — that a guest reading JITCTRL2
after a fast-path interrupt sees bit 29 **set** — is still unverified, and the
acknowledge working does not verify it: the read-modify-write sets bit 29
unconditionally, so it would behave identically if the read returned 0. Proving
it needs one boot that reads the register and reports the bit.

The `[exter] LEVEL6 entry` trace in `Platform_Autovector()` is compiled out by
`PLATFORM_TRACE_BRINGUP` and has never appeared in any run, before or after, so
it is not available as a witness either.

# The open question this hands to Rigel

`INTF.IPL` is now exclusively the Amiga domain's, and **`patches/emu68/0001`
stays** — it is what makes the field mean anything on a standalone build. What
is *not* settled is whether it has the right shape, because the two design
documents disagree:

| | mechanism for Rigel's IPL |
|---|---|
| `docs/New_emu68.md` §15 | **push** — Rigel resolves the level and writes `INTF.IPL` (the PiStorm32 branch `0001` selects) |
| `docs/Rigel_integration.md` §25-26 | **pull** — `rigel_get_ipl()` exposes the current level, "queried after Rigel advancement" (the `PISTORM_CLASSIC` branch `0001` routes away from) |

The pull shape has the better argument, and one part of it is already
normative: `Rigel_integration.md` section 28 says accepting the exception
**MUST NOT** clear the Rigel source, which `0001`'s auto-clear of `INTF.IPL`
does. Section 29 then requires the effective level to be re-evaluated whenever
either domain changes — which a query gives and a latched field does not.

It cannot be a pure pull either way: the arbitration only runs when
`INT64 != 0`, so something must still set a byte in `INTF` to open that gate.
Flag to open the gate, query for the authoritative level — which is exactly
what `PISTORM_CLASSIC` does. What looked like a quirk of old hardware is the
correct decomposition for a source that *holds* a level instead of delivering
an event.

None of that has to be decided now. It is recorded because this is the change
that hands `INTF.IPL` over, and the next person should not have to rediscover
that the two documents point in different directions.

# What is left

1. **Confirm bit 29 reads back set** — one boot, closes `ISSUE-0001` properly.
2. **Decide push vs pull for Rigel**, when Rigel exists. Not before.
3. **Watch this under a second consumer.** USB would be the first DMA
   consumer and the second interrupt consumer on this port; whether one latch
   and one level-6 line hold up with disk, USB and Bluetooth together is the
   thing this change was made early enough to find out about.

# Notes

**This is inside the standing freeze.** Nothing is added: it moves an existing
mechanism onto the channel the design record already chose, and removes a
dependency on a transitional one.

**Do not read "it boots" as "interrupts are correct".** It establishes
delivery and acknowledge. It says nothing about behaviour under load, under
concurrent sources, or on real hardware, none of which has been measured.

# Execution log

- 2026-08-17 — Implemented, built and booted. The user ran it twice
  independently and both reached the icons. `setup.sh --verify` clean at
  aros 23 / emu68 10.
- 2026-08-17 — Opened from the question of whether Emu68 has an `INTF.ARM`
  path at all. Reading the arbitration showed it does, and reading the JIT's
  MOVEC emitters showed the acknowledge it needs was already there.
