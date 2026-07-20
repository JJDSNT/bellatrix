---
id: ISSUE-0070
title: "AROS stalls at lowlevel.library on the bare-metal product"
status: todo
priority: critical
type: bug
owner: agent
created_at: 2026-07-20
updated_at: 2026-07-20
tags: [aros, lowlevel, regression, qemu, baremetal, musashi, stabilization]
related_files:
  - AI_context/issues/ISSUE-0065.md
  - AI_context/issues/ISSUE-0064.md
  - AI_context/issues/ISSUE-0058.md
  - tests/integration/qemu_bisect_boot.sh
  - src/runtime/core_chipset.c
  - src/machine/machine_rigel_step.c
---

# Symptom

AROS does not get past `lowlevel.library` (observed to 2000 frames) on the
**bare-metal product**: QEMU, single-core, legacy boards mode
(`BELLATRIX_EMU68_BOARDS_MODE=legacy`). Originally reported 2026-07-17 against
Musashi 68040; Jaime confirmed on 2026-07-20 that it is **not backend-specific**
— it occurs with Musashi as well as Emu68.

This is one of the two behaviours Jaime expects the stabilization phase to
clear (the other is ISSUE-0065). Raised to `critical` on 2026-07-20 at his
explicit request.

# Scope — the harness cannot reproduce it, and now we know why

Confirmed: the harness has **no** regression — see the prime suspect above:
the harness backend carries the ISSUE-0026 fix that the product backend
lacks. Harness AROS boots deep through
the InitResident chain (expansion → exec → … → trackdisk.device), well past
where the product stalls.

The harness (POSIX x86, "harness zorro2 bus") and the QEMU product (bare-metal
AArch64, PAL raspi3, MMU, startup) differ across the entire platform layer even
though both use the Musashi core. **A harness pass does not clear this path,
and a harness run cannot reproduce this bug.** Time spent there is wasted; this
has already been established once.

# What it is probably not

Almost certainly not caused by the 2026-07-17 Z3/board work. The only shared
code that merge touched on this path is `cpu_bridge_classify()`, which is
byte-identical for AMIGA_LOW (≤24-bit) and returns the same open bus above
24 bits when no Z3 board is registered — and legacy mode registers none. The
`vectors.inc` cast fix only affects boards-mode board reads.

More likely the pre-existing QEMU-product boot class of ISSUE-0064 / the
2026-07-16 bisect. Note the trap: the boot oracle's "single-core musashi boots
at HEAD" verdict only checks **early** liveness (ExecBase non-null and a
resized display). It does **not** check that lowlevel.library is passed, so a
stall there can be long-standing rather than new, and the oracle will report
GOOD throughout.

# Where to look

`lowlevel.library` init probes potgo / CIA / gameport. A hang there points at
chipset (Rigel) hardware reads on the bare-metal path, not at board/bus code.

That places it on the same axis as ISSUE-0065 and ISSUE-0064: **shared
integration semantics between the CPU and the chipset**, not backend
instruction generation. See "Shared-semantics axes" below.

# Shared-semantics axes (recorded 2026-07-20)

Structural work on 2026-07-20 (commits 1544606…4887ca8) did not change
behaviour — it was verified statement-by-statement to preserve it — so it
neither fixed nor could have fixed this. What it did do is make two facts
explicit that were previously hidden by file layout, and both are candidate
axes for this bug and for ISSUE-0065:

1. **There is exactly one implementation of guest memory topology, and it is
   Emu68's** (`bellatrix_emu68_attach_rom_and_ram` /
   `bellatrix_emu68_map_guest_memory`, declared in `src/cpu/emu68/bellatrix.h`).
   The Musashi product build runs on it. So "Musashi vs Emu68" is *not* a clean
   A/B: both share the guest address-space layout, including the chip RAM
   mirror and the ext-ROM probe window.
2. **Chipset time advances through two different paths** — `rigel_step_until()`
   on Core 2 (multicore, `src/runtime/core_chipset.c:889`) versus
   `bellatrix_machine_advance()` driven by the CPU progress hook (single-core,
   `src/machine/machine_rigel_step.c:909`). ISSUE-0064 already showed this
   duality producing a single-core-specific IPL delivery defect.

# PRIME SUSPECT — the ISSUE-0026 fix never reached the product (2026-07-20)

**This symptom has happened before, was root-caused, and was fixed — but the
fix was applied only to the harness backend.**

`AI_context/consolidated/history/ISSUE-0026.md` documents the identical
symptom (AROS InitCode reaching `lowlevel.library` and never getting to the
negative-priority residents) with a proven mechanism:

1. `lowlevel.library` Init calls `CreateMsgPort()` then deliberately
   `FreeSignal(mp_SigBit); mp_SigBit = -1` — a port with no signal bit.
2. `DoIO(IND_ADDHANDLER)` is always non-quick, so it `PutMsg`es to the
   input.device command port. The design only works if the `Signal` preempts
   immediately to the higher-priority input.device task.
3. Musashi's `m68k_set_irq()` only *stores* the level.
   `m68ki_check_interrupts()` runs at the start of `m68k_execute()` or when an
   instruction writes SR — so an IPL raise arriving mid-timeslice is not taken.
4. ~35 instructions later the bootstrap reaches `WaitIO` → `Wait(0)`, whose
   internal `Disable` drops INTENA, Paula lowers the IPL, and **the interrupt
   is rescinded before it is ever taken**. Lost preemption → deadlock, with
   `mp_SigBit = -1` meaning nothing can wake it.

The 2026-07-03 fix was to call `m68k_end_timeslice()` when the level rises, so
the IRQ is taken at the next instruction boundary as real hardware would.

**It was applied to `tools/harness/musashi_backend.c` only** (still there at
line ~2878, with the ISSUE-0026 comment). The bare-metal product backend,
`src/cpu/musashi/musashi_backend.c`, has:

```c
static void musashi_set_ipl(void *ctx, int level)
{
    (void)ctx;
    m68k_set_irq((unsigned int)level);
}
```

No `m68k_end_timeslice()`. The product carries the exact defect ISSUE-0026
fixed.

**This explains the otherwise strange scoping of this issue** — "the harness
boots deep past lowlevel, the product stalls at lowlevel". That was read as
the harness being too different a platform to reproduce the bug. The simpler
reading is that **the harness has the fix and the product does not.**

## What this does and does not explain

Jaime confirms the stall also occurs under **Emu68**, which does not use
either Musashi backend, so this cannot be the whole story. Two readings, both
worth testing:

- Two defects with one symptom: this one on Musashi, something else on Emu68
  (the `MainLoop()` suspects below, or the shared IPL publication path).
- One defect with two expressions: `m68k_end_timeslice()` is Musashi's way of
  saying "take the pending IRQ at the next instruction boundary". If Emu68's
  path has an analogous *latency* between IPL publication and exception
  delivery, the same rescind-before-taken race applies. ISSUE-0064 already
  documented a single-core-specific IPL delivery defect on Emu68.

## First experiment

Port the ISSUE-0026 fix to `src/cpu/musashi/musashi_backend.c` and re-run AROS
on the product with Musashi single-core. It is a three-line change against a
documented root cause. If Musashi then passes lowlevel and Emu68 still stalls,
the two-defect reading is confirmed and the Emu68 half is cleanly isolated.

# Named suspects shared with ISSUE-0065 (2026-07-20)

Diffing `c7745bc` (2026-07-09) against HEAD found three semantic changes new
in the window, all inside `MainLoop()` via
`patches/0003-bellatrix-execution-loop.patch`: `1e3b361` (report on every pass
while STOPPED), `995e79d` (`CYCLE_COUNT += 44u` per interrupt delivery) and
`df4559c` (retire STOP before stacking its return PC). See ISSUE-0065 for the
full table and reasoning.

**Relevance here is weaker than for ISSUE-0065 and must not be assumed.**
This issue's reported configuration is *Musashi*, which never executes
`ExecutionLoop.c`. So these three cannot explain a Musashi-only stall. They
are recorded here only because both issues sit on the CPU→chipset time axis,
and because the reported configuration should be re-confirmed: if the AROS
lowlevel stall also occurs on **Emu68** single-core, these become live
suspects for it too.

First cheap step for this issue is therefore: confirm whether the stall is
Musashi-only or also reproduces under Emu68.

# How to settle it

Bisect the actual product boot (not the harness):

```bash
BELLATRIX_CPU_BACKEND=musashi BELLATRIX_MULTICORE_BUILD=0 \
  ROM=src/roms/aros.rom ./tests/integration/qemu_bisect_boot.sh
```

against pre-merge `205c27f`. **The script must be adapted first**: its pass
criterion checks early liveness, not lowlevel.library. Make it grep the serial
log for lowlevel and what should follow it, or the bisect will report GOOD on
every commit.

Gotcha when using that script: it injects an ExecBase probe into
`src/machine/machine_rigel_trace.c` and does not clean up. `git checkout --`
that file after every run.

# History

- 2026-07-17 — reported; Jaime chose to finish the Z3/board_registry work
  first and park this for a focused stabilization phase.
- 2026-07-20 — raised to `critical`; promoted from a session note to a tracked
  issue at Jaime's request, together with ISSUE-0065.
