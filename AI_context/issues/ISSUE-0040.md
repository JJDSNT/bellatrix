---
id: ISSUE-0040
title: "The SDHOST data path copies every block twice, and nobody has measured it"
status: backlog
priority: medium
type: investigation
owner: unassigned
created_at: 2026-08-17
updated_at: 2026-08-17
tags:
  - sdcard
  - sdhost
  - dma
  - performance
  - emu68
blockers:
related_files:
  - external/aros/arch/arm-native/soc/broadcom/2708/sdcard/sdcard_sdhost_bus.c
  - aros/arch/m68k-emu68/soc/sdcard/mmakefile.src
  - patches/aros/0024-sdcard-report-what-the-pio-data-loop-costs.patch
  - AI_context/issues/ISSUE-0013.md
---

# Summary

The card now boots on the BCM2835 SDHOST controller with DMA (ISSUE-0013). What
the data path costs is unknown, and there are two reasons to think it is
leaving something on the table.

This issue is deliberately *not* "make the card faster". It is three separate
questions, in the order they should be answered, because answering them out of
order is how the previous round of this work wasted an afternoon.

# 1. There is no measurement of the new path at all

`patches/aros/0024` brackets the PIO word loop in `rom/devs/sdcard/sdcard_bus.c`
and reports per megabyte. On the Arasan backend it gave the only number this
project has:

```
[SDBus00] PIO: 7168 KiB in 13110 ms, 14339 blocks     (QEMU, idle host)
```

**On SDHOST that instrument is silent** — zero `PIO:` lines in a full boot,
because the SDHOST backend has its own `FinishData` and never enters that loop.
So the current state is: SDHOST *works*, and whether it is faster is not known.
Comparing 13.1 s against "not measured" is not a comparison.

**First step, and it blocks the other two:** instrument the SDHOST path in the
same unit — time and bytes, reported per megabyte — so the two backends produce
numbers that can be put side by side. The switch in
`aros/arch/m68k-emu68/soc/sdcard/mmakefile.src` (`SDCARD_BACKEND`) exists
precisely so the same card and the same boot can be run both ways.

# 2. Every block is copied, because of a diagnostic left switched on

`sdcard_sdhost_bus.c:457-461`:

```c
/* Diagnostic: always route through the bounce buffer so every
 * transfer uses one well-aligned, known-good DMA address. Direct
 * DMA is only used as a fallback if the bounce buffer is too
 * small or unavailable. */
```

So the bounce is not the misaligned-buffer fallback it is written as — it is
**unconditional**, in both directions:

| line | direction | copy |
|---|---|---|
| `:468` | write | caller buffer → bounce, before the DMA |
| `:513` | read | bounce → caller buffer, after the DMA |

Every 512-byte block therefore pays a 512-byte RAM-to-RAM copy on top of the
DMA. Removing that is worth more than making the copy faster: DMA straight into
the caller's buffer copies nothing at all.

**What has to be established before touching it**, since upstream chose this
deliberately: what the direct path requires of the caller's buffer (alignment,
and whether it may cross whatever boundary the engine cares about), and whether
AROS's filesystem buffers satisfy it. `dma_bounce` is 32-byte aligned by hand
in the driver, which is a hint about the requirement rather than an answer.

# 3. The copy itself is word-at-a-time here, and NEON is not the alternative

`sdhost_neon_copy()` moves 64 bytes per iteration through `vldm/vstm`, guarded
by `#if defined(__arm__)`. On this target that is false and the `#else` branch
runs: a plain `ULONG` loop, four bytes at a time.

**The NEON path is not something this port can have, and not because of a
conflict.** The guest is m68k and cannot emit NEON at all; only Emu68's own
AArch64 can. So the guarded block is simply dead code here, not a collision.

(There *is* a real question about NEON on the Emu68 side of the boundary --
`external/emu68/CMakeLists.txt:36-39` reserves `v19`-`v26` for the JIT, and
ISSUE-0038's root cause was a clobber of `x12`/`v28` because GCC ignores
`-ffixed` in the prologue. That matters if anyone ever accelerates this copy
*in Emu68*. It has nothing to do with the guest-side code here.)

The m68k equivalent of the NEON block is **`movem.l`**, which moves up to 16
registers in one instruction -- 64 bytes, the same chunk size the NEON path
uses. Under a JIT the unit of cost is the instruction rather than the byte,
which is the same reasoning that made `CPUSHP` beat a `CPUSHL` line loop by
about eleven seconds of boot time in `CacheClearE` (ISSUE-0019). A 512-byte
block is 128 `move.l` instructions or 8 `movem.l` ones.

**Not yet checked, and it decides whether this is worth anything:** how Emu68
translates `movem.l`. If it breaks the instruction into individual loads the
advantage evaporates, and the JIT's own translation is the thing to read --
`external/emu68/src/M68k_LINE4.c` -- not something to assume.

# Order, and why it matters

1. **Measure the SDHOST path** in the same unit as `0024`. Until then every
   opinion here is unpriced.
2. **Then remove the unconditional bounce**, if the alignment requirement
   allows it. This is the large term and it deletes work rather than
   accelerating it.
3. **Only then consider `movem.l`**, and only after reading how Emu68
   translates it. If step 2 succeeds the copy may not exist any more.

Doing 3 before 1 would optimise a copy that step 2 might delete.

# Notes

**This sits inside the standing freeze as measurement and as making what exists
faster, not as new functionality.** The card already boots this way.

**Both backends still build**, and the switch is one line in
`soc/sdcard/mmakefile.src`. That was kept deliberately: a regression on one has
to be answerable against the other without a bisect.

# Execution log

- 2026-08-17 — Opened while the SDHOST port was being verified. The boot
  succeeded and the DMA probe reported channel 9 instead of its usual 8,
  because the SDHOST backend had already taken channel 8 -- which is how the
  DMA path was confirmed to be live. No performance number was taken, which is
  what this issue is mostly about.
