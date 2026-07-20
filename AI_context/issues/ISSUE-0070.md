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
**bare-metal product**: QEMU, Musashi 68040, single-core, legacy boards mode
(`BELLATRIX_EMU68_BOARDS_MODE=legacy`). Reported 2026-07-17.

This is one of the two behaviours Jaime expects the stabilization phase to
clear (the other is ISSUE-0065). Raised to `critical` on 2026-07-20 at his
explicit request.

# Scope — do NOT try to reproduce in the POSIX harness

Confirmed: the harness has **no** regression. Harness AROS boots deep through
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
