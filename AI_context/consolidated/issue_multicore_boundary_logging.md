// AI_context/consolidated/issue_multicore_boundary_logging.md

# Issue: Cross-core boundary logging (CORE1↔CORE2, CORE3↔CORE2)

## Status: resolved (2026-06-21)

Replaced `newlogs.md` (deleted from repo root), which proposed per-domain log
tags and independent enable flags for tracing event flow across "cores"
named CORE0-CPU/CORE1-AGNUS/CORE2-PAULA/CORE3-IO. That mapping didn't match
the architecture: Agnus, Paula, CIA and Denise are not separate cores — they
all live inside Rigel (`external/rigel`), single-threaded on **Core 2**. The
real split is Core 0 = host/arbiter, Core 1 = CPU backend, Core 2 = Rigel
chipset, Core 3 = physical IO (USB/BT). See `[[project_multicore_domains]]`.

## Major correction made during implementation

Source-level `grep` is not proof of what a build actually contains — verify
against the built binary (`strings`) before trusting a code-reading
conclusion. The first analysis pass claimed the CORE2→CORE1 IPL boundary was
"already covered" by `XCORE_LOG("PAULA->CPU", ...)` in `core_cpu.c:19`. That
was wrong: built with `BELLATRIX_MULTICORE_BUILD=1 BELLATRIX_LOGS=1` and ran
`strings` on the output `Emu68.img` — **`core_cpu.c` is not compiled into
the bare-metal build at all.**

`cmake/bellatrix-variant.cmake` added `runtime/core_cpu.c`, `runtime.c`,
`runtime_core.c`, `event.c`, `mailbox.c`, `affinity.c`, `clock.c`, `stats.c`,
`sync.c` to `BASE_FILES`, then immediately `list(REMOVE_ITEM ...)`d the same
files a few lines later (only `core_chipset.c`/`core_io.c` were re-added
afterward). The entire `RuntimeCoreCPU` abstraction, and a generic
`RuntimeEventQueue`/mailbox skeleton, is dead code, deliberately excluded —
not just "unintegrated" as first assumed.

The real Core 1 (CPU) entry point is `bellatrix_core1_entry()` in
`src/host/raspi3/pal_core.c`, which spins on a function pointer set by
`PAL_Core_LaunchCpu()` and runs Emu68/Musashi's own loop directly — no
struct, no init/shutdown/reset lifecycle.

**The real, live, already-existing IPL/INTREQ/INTENA/DMACON/BPLCON0/frame
trace facility is `RigelTrace` (`g_rtrace`) in
`src/machine/machine_rigel_trace.c`** — `[RIGEL-IPL]`, `[RIGEL-IRQ]`,
`[RIGEL-DMACON]`, `[RIGEL-BPLCON0]`, `[RIGEL-FRAME]`, all cycle-stamped
(`cyc=%llu` from `rigel_step_result_t.time`), plus PC context via
`bellatrix_debug_cpu_pc()`. This is a separate, more mature facility than
`src/debug/core_log.h`, enabled via `BELLATRIX_RIGEL_TRACE` (harness env
var) / `BELLATRIX_RIGEL_TRACE_BUILD` (bare-metal compile flag) /
`bellatrix_machine_rigel_trace_enable()`. It already covered most of what
the original CORE2→CORE1 and CORE2→display boundaries wanted.

`run.sh` already unifies both flags under the single `BELLATRIX_LOGS`
toggle (around lines 404-558): it derives `TRACE_LOGS` from `BELLATRIX_LOGS`
and exports both `BELLATRIX_RIGEL_TRACE`/`BELLATRIX_RIGEL_TRACE_BUILD` *and*
feeds `BELLATRIX_CORE_LOG` via `scripts/build.sh`. The "no separate TUI
toggle" constraint holds across **both** logging systems even though
they're independent code paths — confirmed end-to-end: TUI "Logs: ON" +
"Multicore: ON" → `run.sh` exports `BELLATRIX_LOGS`/`BELLATRIX_MULTICORE_BUILD`
→ falls into the `BUILD_KIND=bellatrix` branch, which always rebuilds via
`setup.sh`+`build.sh` before running qemu → `scripts/build.sh` forces
`MULTICORE_LOGS=0` unless `MULTICORE_BUILD=1`, then passes
`-DBELLATRIX_CORE_LOG=ON` to cmake.

## What was implemented

Fixed a pre-existing bug found in passing: `core_cpu.c` mislabeled its own
init/shutdown/reset as `CORE0_LOG` (tag `[CORE0-HOST]`) despite being the
CPU (Core 1) domain. Changed to `CORE1_LOG`. Has zero runtime effect today
since `core_cpu.c` isn't compiled — kept for correctness if that abstraction
is ever revived.

Added three new `XCORE_LOG`/filtered-log call sites, all gated by the
existing `BELLATRIX_CORE_LOG` define (no new flag, no new TUI entry):

1. **CORE1 → CORE2, cycle publish** — `src/runtime/core_chipset.c`,
   `bellatrix_runtime_publish_cpu_cycles()`. Logs `m68k`/`cck`/`target` per
   publish. Also simplified the function: unified the profile-only and
   non-profile branches of the `atomic_fetch_add_explicit` call (both did
   the same add, only one captured the return value) since the log needed
   the post-add target either way.
2. **CORE3 → CORE2 (via machine), HID delivery** —
   `src/machine/machine_rigel.c`, `machine_keyboard_drain_rigel()`. Logs the
   byte actually delivered to CIA-A SDR plus remaining queue depth, right
   after `cia_receive_sdr()`. Left the pre-existing always-on
   `kprintf("[KBD] ...")` in `bellatrix_machine_keyboard_rawkey()` untouched
   — a different (ungated) diagnostic that predates this issue and may
   still be load-bearing for active BT/USB keyboard debugging.
3. **CORE1 ↔ CORE2, critical MMIO writes** — `src/cpu/cpu_bridge.c`,
   `bellatrix_bridge_cpu_write()`. Added a filtered allow-list
   (`cpu_bridge_log_critical_write()`, compiled out entirely when
   `BELLATRIX_CORE_LOG` is undefined) logging writes to DMACON (0x096),
   INTENA (0x09A), INTREQ (0x09C), COPJMP1/2 (0x088/0x08A), BLTSIZE (0x058)
   — the registers flagged as the open "MMIO crítico" gap in
   `[[project_multicore_domains]]`. CPU-side view (the instant the write
   happens); `RigelTrace`'s `[RIGEL-DMACON]`/`[RIGEL-IRQ]` is the
   chipset-side view (the instant Rigel's step result reflects it, with a
   cycle timestamp). Complementary, not redundant.

Dropped from the plan: a "CORE2 → CORE0/display, frame ready" boundary.
`bellatrix_machine_on_frame_ready()` → `machine_present_frame_from_rigel()`
is a synchronous call within Core 2's own step loop writing pixels into a
VC4 framebuffer for the GPU to consume asynchronously — no second software
core involved, not a real cross-core boundary, and already covered by
`[RIGEL-FRAME]` in `RigelTrace`.

Fixed two stale build-output messages that were actively printing the
**old, obsolete** core mapping (matching the abandoned `novo_sprint.md`
Core1=GFX/Core2=Paula split): `scripts/build.sh:146,157` and
`cmake/bellatrix-variant.cmake:39`. Now print the real mapping (Core1=CPU,
Core2=Chipset/Rigel) and the correct tag list.

## Cmake source-list cleanup (follow-up, same session)

`cmake/bellatrix-variant.cmake` used to `list(APPEND BASE_FILES ...)` a
large batch including the legacy pre-Rigel chipset files
(`src/chipset/cia/*`, `agnus.c`, `paula*.c`, `denise.c`, `floppy_drive.c`,
...) and the dead `src/runtime/*` orchestration files (`runtime.c`,
`core_cpu.c`, `event.c`, `mailbox.c`, `affinity.c`, `clock.c`, `stats.c`,
`sync.c`), then immediately `list(REMOVE_ITEM BASE_FILES ...)`d the exact
same two groups a few lines later, then re-`APPEND`ed
`core_chipset.c`/`core_io.c`. Net effect was correct but required two large
lists to stay in sync by exact string match — any future addition to the
legacy/dead families that forgot the matching `REMOVE_ITEM` entry would
silently enter the build.

Worse: confirmed `src/chipset/` **doesn't exist on disk at all** — already
deleted in the Rigel migration. The `APPEND` block referenced paths to files
that are physically gone, surviving only because `REMOVE_ITEM` stripped them
before CMake ever tried to open them.

Removed the dead entries directly from the `APPEND` block and deleted the
whole `REMOVE_ITEM` block; `core_chipset.c`/`core_io.c` now appear once, in
their natural place.

## Validation done

- `BELLATRIX_MULTICORE_BUILD=1 BELLATRIX_LOGS=1 BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1 ./scripts/build.sh` (and again with `clean`) — exit 0.
- `strings Emu68.img` — confirmed all 3 new log format strings present
  (`[XCORE-CPU->CHIPSET] m68k=...`, `[XCORE-IO->CHIPSET] kbd byte...`,
  `[XCORE-CPU->CHIPSET] DMACON write...` etc.), confirmed `CORE1-CPU`
  strings absent (expected — dead code) and `CORE2-CHIPSET`/`CORE3-IO`
  init/shutdown strings present (expected). Same 13 boundary-log strings
  present before and after the cmake cleanup — behavior-preserving.
- QEMU boot smoke test (`-display none`, ~10-12s bounded run): boots past
  bootstrap, wakes CPU1/CPU2/CPU3, reaches the launcher's framebuffer logo
  stage with no fault/crash, identical before and after the cmake cleanup.
  Did not reach a kickstart ROM (launcher needs interactive SD selection,
  out of scope for this change).
- Separate Musashi-backend build (`BELLATRIX_CPU_BACKEND=musashi`) to
  confirm the shared cmake file still works for both CPU backends — exit 0.

## Files touched

- `src/runtime/core_cpu.c` (CORE0_LOG→CORE1_LOG fix; dead code, see above)
- `src/runtime/core_chipset.c` (boundary log + atomic-capture simplification)
- `src/machine/machine_rigel.c` (boundary log + `#include "debug/core_log.h"`)
- `src/cpu/cpu_bridge.c` (boundary filtered log)
- `scripts/build.sh` (stale core-mapping echo fix, lines 146/157)
- `cmake/bellatrix-variant.cmake` (stale core-mapping message fix, line 39;
  removed dead legacy-chipset/runtime entries and the matching
  `REMOVE_ITEM` block)

## What's still open

See `AI_context/issue_core_log_vs_rigeltrace.md` for the unresolved design
questions this work surfaced (whether to unify `core_log.h` and
`RigelTrace`, allow-list scope, Rigel's internal trace).
