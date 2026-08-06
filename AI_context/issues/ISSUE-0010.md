---
id: ISSUE-0010
title: "Gate the host-IRQ delivery mechanism: INTENA shadow or PiStorm's IPL injection"
status: backlog
priority: high
type: research
owner: unassigned
created_at: 2026-08-06
updated_at: 2026-08-06
tags:
  - emu68
  - irq
  - patches
  - architecture
blockers:
related_files:
  - docs/irq.md
  - patches/emu68/0001-emulate-amiga-interrupt-registers.patch
  - external/emu68/src/aarch64/vectors.c
  - external/emu68/src/ExecutionLoop.c
  - aros/arch/m68k-emu68/platform/platform.c
  - aros/arch/m68k-emu68/boot/selftest.c
---

# Summary

Every host interrupt reaching the guest today goes through Emu68's emulated
Paula: the core-0 fast path tests an `INTENA` shadow and, if the guest has armed
INTEN+EXTER, raises m68k level 6. Emu68 also carries a second, much smaller
mechanism — `INTF.IPL`, the interface a real PiStorm uses to hand the CPU a
level that some external controller has already decided on. That one needs no
INTENA at all.

This issue is to build the **switch between them** and then measure both, rather
than continuing to carry one path with no way to try the other.

# Problem

## The path that exists

`curr_el_spx_irq` in `external/emu68/src/aarch64/vectors.c:148-170` decides
inline, in assembly:

```
ldrh w0, [x1, #INTENA]      // INT_shadow.INTENA
and  w0, w0, #0x6000        // INTEN | EXTER
cmp  w0, #0x6000
mov  w0, #1
strb w0, [x1, #ARMPending]  // always recorded
b.ne 1f                     // not enabled -> do not raise
mov  w0, #6
strb w0, [x1, #INTF.ARM]    // level 6 into the m68k context
```

`patches/emu68/0001` then serves `INTENA`/`INTENAR`/`INTREQ`/`INTREQR` out of
that shadow (`vectors.c:387-406`), because standalone there is no Paula to
answer. The guest arms once and acknowledges every entry — see
`aros/arch/m68k-emu68/platform/platform.c:55-67`, `emu68_exter_enable()` and
`emu68_exter_ack()`.

Two costs follow. Every arm and every acknowledge is a page fault into
`SYSWriteValToAddr()`, and the whole `0xdff000` page must be treated as MMIO.
More important than either: **Emu68 owns INTENA/INTREQ**, and the moment this
project has a chipset there will be two candidate owners of the same two
addresses. `docs/irq.md` records that as undecided.

## The path that is already there and unreachable

`ExecutionLoop.c:325-378` arbitrates the pending level. Its PiStorm branch uses
`INTF.IPL` directly:

```c
#if defined(PISTORM)
    if (ctx->INTF.IPL > level) level = ctx->INTF.IPL;
#else
    if (ctx->INTF.IPL) {
        ipl_level = GetIPLLevel();   /* return 0 outside PISTORM_CLASSIC */
        if (ipl_level > level) level = ipl_level;
    }
#endif
```

A stock build lands in the `#else`, where `GetIPLLevel()` is a `return 0` stub
(`ExecutionLoop.c:296`). Writing `INTF.IPL` therefore opens the `if` and then
loses the level. Reaching it is roughly thirteen lines against ~100 for the
shadow series, and the SR-mask arbitration around it already behaves like a real
68k.

No register is emulated, no address space is reserved, and no INTENA exists to
be armed — which is the whole point of the alternative, and also its cost: an
IPL is a level, not a register, so **whoever writes it must already have made
the enable/acknowledge decision**. An unmodified Amiga guest gets nothing to
program; writes to `0xdff09a` land in RAM.

## Why this is worth building rather than choosing on paper

It was tried in the previous incarnation of this project and **did not settle
the problem**: that build patched the arbitration to accept `INTF.IPL`, and
single-core Emu68 still hung in Exec idle at `pc=0xfc0f90` polling INTREQ,
across 400 chipset frames and ~125M instructions with `ipl=0` and `int32=0`
throughout — VERTB generated underneath and never delivered. The multicore path
delivered correctly over the same interface.

The lesson `docs/irq.md` draws from that is the reason for this issue:
**choosing the mechanism does not settle delivery.** The failure was not in
`INTF.IPL` as an interface but in *when* the write happens relative to the JIT's
execution window. That question is separate, and it can only be answered by
running both paths against the same workload — which today is impossible,
because only one of them can be built.

# Goal

One tree that can be built either way, with the choice visible in the boot log,
so the two paths can be A/B measured on the same ELF and the same card.

# What is left

**1. Decide where the switch lives.** Two axes, and they are not the same
question:

- *Emu68 side* — a build-time option (a new patch in `patches/emu68/`, guarded
  so the default remains today's shadow path) that makes the fast path store a
  level into `INTF.IPL` instead of testing `INTENA` and storing `INTF.ARM`, and
  makes the `#if defined(PISTORM)` arbitration branch reachable in a stock
  build. Build-time is the honest granularity here: the fast path is hand-written
  assembly and a runtime test in it costs a load and a branch on every IRQ.
- *Guest side* — under the IPL path, `emu68_exter_enable()` must not run and
  `emu68_exter_ack()` must not run, so `platform.c` has to know which image it
  is under. Preference: **do not make the guest a build variant.** One AROS ELF
  should work with either Emu68 image, selected by a boot argument, with
  autodetection as the fallback — the guest can already probe: under the shadow
  path `INTENAR` at `0xdff01c` reads back the mask Emu68 holds
  (`platform.c:375-376`, and `expansion/memorytest.S:91-98` already probes
  exactly this), and under the IPL path it reads RAM.

**2. Answer the de-assert question before writing code.** This is the real
design work, not the plumbing. `INTF.ARM` is edge-shaped and the guest drops it
by acknowledging EXTER. `INTF.IPL` is level-shaped: something has to lower it,
and there is no Paula to do so. Candidate answers, to be written down and chosen:

- Emu68 clears `INTF.IPL` when the m68k takes the exception (edge semantics on a
  level interface — simple, and wrong if two sources are pending);
- the ARM side clears it when the peripheral is acknowledged, which requires the
  guest's `Platform_Autovector()` to signal back through something that is not
  `0xdff09c`;
- the `HOSTIRQ` MOVEC sketch in `docs/irq.md` §3, which is exactly a
  guest-visible ack that costs no page fault.

Whatever is chosen must also say what happens to `krnRunIRQHandlers()`'s loop in
`aros/arch/m68k-emu68/platform/bcm283x/interrupt_controller.c` — the driver-side
decode does not change, only how it is entered and left.

**3. Measure.** Same ELF, same card, both images, per the discipline in
`CLAUDE.md`: at least 3 serial runs each, alternating, on an idle machine
(`ps -eo cmd | grep -c '[q]emu-system'` must be 0 first), card regenerated
between runs (ISSUE-0009), classified by dominant colour from `screendump`. The
question the measurement answers is not "which is faster" but **"does either one
deliver reliably"** — the intermittency in ISSUE-0007 is still unattributed, and
this is one of the few structural suspects.

**4. Record the outcome in `docs/irq.md`**, whichever way it goes. That document
already lays out the three mechanisms and states the choice is open; it is the
place the answer belongs.

# Decisions taken

The shadow path stays the default. This issue adds an alternative and the
ability to measure it; it does not switch the project onto the IPL path on the
strength of the argument.

# Acceptance criteria

- [ ] `patches/emu68/` carries a gated IPL-injection path, off by default,
      applied through `setup.sh` and verified by `setup.sh --verify`
- [ ] The de-assert protocol is written down before it is implemented
- [ ] The guest selects its behaviour without a second ELF (boot argument, or
      `INTENAR` probe), and logs which path it took
- [ ] Both images boot to the Workbench with icons, ≥3 serial runs each
- [ ] `docs/irq.md` records the result and, if one path is chosen, why

# Notes

The shadow path has two known rough edges that any comparison has to hold
constant, both documented in `docs/irq.md`: the write side of the
`INTENA`/`INTREQ` intercepts has no access-size gate, and a 32-bit access
spanning `INTENAR`+`INTREQR` falls through to memory. See ISSUE-0005.

`INT_shadow`'s field layout is load-bearing — `vectors.c:283-286` passes
`__builtin_offsetof` values into hand-written assembly, and the compiler will
not check a reordering for you.

# Execution log

- 2026-08-06 — opened. Both mechanisms are described in `docs/irq.md`; only the
  shadow one can be built today, and the previous attempt at the other was never
  closed.
