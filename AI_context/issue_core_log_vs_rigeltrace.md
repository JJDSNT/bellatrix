// AI_context/issue_core_log_vs_rigeltrace.md

# Issue: core_log.h vs RigelTrace — unify, bridge, or leave as two views?

## Status: open

## Context

Full implementation history and validation for the cross-core boundary
logging work that surfaced this question is in
`AI_context/consolidated/issue_multicore_boundary_logging.md`. Summary: the
project ended up with **two independent, currently-active logging
facilities** that both cover IPL/INTREQ/INTENA/DMACON territory:

- `src/debug/core_log.h` (`CORE*_LOG`/`XCORE_LOG`) — CPU-side view. Logs the
  instant a write happens (e.g. `cpu_bridge.c`'s critical-register filter)
  or the instant a cross-core event is published (cycle publish, keyboard
  delivery). Gated by `BELLATRIX_CORE_LOG`.
- `RigelTrace` (`g_rtrace`, in `src/machine/machine_rigel_trace.c`) —
  chipset-side view. Logs the instant Rigel's step result reflects a change
  (`[RIGEL-DMACON]`, `[RIGEL-IRQ]`, `[RIGEL-IPL]`, `[RIGEL-FRAME]`), with a
  cycle timestamp (`cyc=%llu`) and PC context. Gated by
  `BELLATRIX_RIGEL_TRACE`/`BELLATRIX_RIGEL_TRACE_BUILD`.

Both are already unified under the single `BELLATRIX_LOGS` toggle upstream
(`run.sh`), so there's no UX problem today — this is purely an internal
design question about whether having two systems is the right end state.

## Question 1 — Unify, bridge, or leave as-is?

Three options, no decision made yet:

- **(a) Leave as complementary views.** Cheapest, zero migration risk. Cost:
  when debugging a real cross-core timing bug, you'd grep two different log
  formats and manually line up timestamps (CPU side has no cycle stamp
  today; chipset side does).
- **(b) Give both a shared correlation key.** E.g. have `cpu_bridge.c`'s
  critical-write log also stamp the CPU-side cycle count (would need
  `cpu_bridge.c` to gain visibility into a cycle counter it doesn't have
  today — it runs decoupled from `RuntimeCoreCPU`, see consolidated issue).
  Lets you reconstruct one ordered timeline: "CPU wrote DMACON at cycle X,
  Rigel's step result reflected it at cycle Y" — directly answers the class
  of bug newlogs.md originally worried about.
- **(c) Retire one.** `RigelTrace` is the more mature, already
  comprehensive one (covers DMACON/BPLCON0/IPL/IRQ/frame with cycle+PC
  context); `core_log.h`'s CPU-side additions are newer and thinner. Folding
  the 3 new `XCORE_LOG` call sites into `RigelTrace`-style logging would
  remove the duplication, but `core_log.h` also covers per-core
  init/shutdown/reset lifecycle logging that `RigelTrace` doesn't and isn't
  scoped to do.

No recommendation banked yet — depends on whether a real cross-core timing
bug actually shows up that needs the correlation (b) would give. Per
`[[feedback_implementation_depth]]`, this leans toward deferring until
there's observed need, not deciding speculatively.

## Question 2 — Should the MMIO critical-register allow-list grow or shrink?

`cpu_bridge_log_critical_write()` (`src/cpu/cpu_bridge.c`) currently logs
writes to DMACON, INTENA, INTREQ, COPJMP1/2, BLTSIZE — sized from the known
"MMIO crítico" gap recorded in `[[project_multicore_domains]]`, not from an
observed bug report. Candidates if real debugging shows the need: COPCON,
BLTCON0/1. Also consider whether reads (not just writes) of any of these
ever matter for a bug class, since the current filter is write-only by
design (writes are where "CPU set it but chipset saw it late" bugs live).

## Question 3 — Should Rigel's own internal trace forward through CORE2_LOG?

`external/rigel/src/chipset/agnus/debug/agnus_trace.c`/`.h` is Rigel's own
trace facility (separate submodule, own test suite). Untouched by this
work. Open question: should Bellatrix forward a subset of Rigel's internal
events through `CORE2_LOG` at the boundary only (to give the bare-metal
build one place to look), or is keeping Rigel's tracing fully internal to
the submodule the right separation of concerns (Rigel doesn't know it's
running inside Bellatrix's core split, and arguably shouldn't).

## Files to revisit when picking this back up

- `src/debug/core_log.h`
- `src/machine/machine_rigel_trace.c` (`RigelTrace`)
- `src/cpu/cpu_bridge.c` (`cpu_bridge_log_critical_write`)
- `external/rigel/src/chipset/agnus/debug/agnus_trace.c`/`.h`
