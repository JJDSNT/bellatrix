# ISSUE-0071 — Chip-bus timing: honest hybrid (deadline + slot) and the blitter undercharge

**Status:** open — slice 1 (blitter channel-count cost) landed behind an opt-in gate.
**Area:** Rigel timing (`external/rigel`), chipset-bound performance & fidelity.
**Relates to:** ISSUE-0063 (Rigel CCK-loop perf), ISSUE-0064 (VERTB idle loop),
`rigel/AI_context/dma_slot_timing.md` ("Approach C"), `docs/timing_test.md` (Copperline oracle).

## Motivation

The timing-test (Copperline ADF, cross-checked on real hardware) measured Rigel
charging the **blitter 2–3× too little emulated time** vs the FS-UAE reference.
The user's proposal was to introduce a "chip bus" for Rigel so that contention
and temporal ordering are *derived from the bus*, instead of compensated with
separate constants in each component.

## Key finding: most of the "chip bus" already exists

Investigation (2026-07-24) showed the proposed architecture is ~60% built:

| Proposed | Reality in Rigel | Verdict |
|---|---|---|
| single temporal authority (`rigel_cck_t now`) | `ctx->chipset.cycles`, read by `rigel_get_time()` | **exists** — do NOT add a second clock |
| deadlines ("how far is it safe to skip") | `rigel_get_deadline()` = min(blitter/beam/vertb/copper/audio/disk/slot) | **exists** |
| bus-owner enum | `agnus_slot_owner_t` (refresh/disk/audio/sprite/bitplane/copper/blitter/cpu) | **exists** |
| per-CCK slot calendar | `slot_scheduler` builds the 227-slot table from DMACON+beam+DDF | **exists** |
| blitter consumes real slots | scheduler dispatches `rigel_agnus_blitter_step_dma(ctx, 1)` per stolen FREE/CPU slot (`slot_scheduler.c`) | **exists** |
| deadline vs detailed seam | `rigel_get_next_deadline` (incl. slot) vs `rigel_get_next_observable_deadline` (excl.) | **exists** |

Note: `rigel_dma_domain_blitter_grants()` (returns `cycles`) is **dead code** — no
callers. The blitter is already slot-dispatched. An earlier hypothesis that the
undercharge lived there was wrong.

## The invariant (the design contract)

Deadline and cycle/slot accuracy are **not two clocks or two modes** — they are one
timeline (`chipset.cycles`) at two resolutions. The rule that lets them coexist
without double accounting:

> **Fast-forward across an interval only when no arbitration decision could differ
> inside it** — no blitter active, no contending DMA channel, no pending CPU chip
> access, no armed copper event.

When that holds, skipping is *exact* (walking CCK-by-CCK would produce identical
state). When it does not, the slot-walk takes over for exactly that stretch, and
there is a single cost function: per-slot. `agnus_slot_scheduler_next_event()`
already computes the skip target; the discipline is to only ever skip idle stretches.

The failure mode to avoid is **two different cost functions mutating the same state**
(a coarse bulk charge racing a fine per-slot charge over the same interval).

## Where the undercharge actually lives

`src/chipset/agnus/blitter/blitter_timing.c :: blitter_estimate_cycles()`:

```c
cycles = width_words * height_lines;   // COPY: 1 bus cycle per word
```

Real Agnus spends **one bus cycle per active DMA channel per word** (USEA/USEB/USEC/
USED in BLTCON0). So:
- A→D copy (2 channels) → real cost 2× this → Rigel 2× too fast.
- cookie-cut A+B+C→D (4 channels) → up to 4× too fast.

That is exactly the measured 2–3× undercharge. The comment already admits "This is
NOT cycle exact." The surrounding slot infrastructure is correct; only this estimate
is wrong — so the fix is surgical, not a new bus.

## The full divergence map (don't tunnel on the blitter)

The timing-test measures **27 rows across six families**. The blitter is only one.
Crucially, they split by *whose* faithfulness they test — chipset-intrinsic rows are
valid Rigel oracles regardless of CPU backend; CPU-colored rows move with the CPU
adapter (and the harness runs Musashi+KS13, a different machine than the FS-UAE
A500+/68EC020 reference, so those rows are not apples-to-apples until the adapter
work lands).

| Family | Rows | What diverges | Owner |
|---|---|---|---|
| **A. CPU instruction ratio** | 4 move, 5 shift, 6 mul, 7 dbra, 28/29 dual-issue, 30 branch-cache | CPU cycle→E-clock faithfulness | CPU backend (Musashi/Emu68) — **deferred cluster** |
| **B. Memory-access timing** | 0 slowR, 1 slowW, 2 chipR, 3 chipW, 13 dbraSlow, 14 dbraChip | cycles per slow/chip access + code-fetch | CPU adapter + bus — **deferred cluster** |
| **C. DMA contention on CPU** | 10 cw1024, 11 cw/6bpl, 12 cw/8spr, 15 combined, 18 cw/3bpl | CPU chip writes not stalled by bitplane/sprite DMA (`cpu_would_stall` unwired) | bus (produces) + CPU adapter (consumes) — **deferred cluster** |
| **D. Interrupt cost & latency** | 16 cw/f, 17 cw/f+VB, 19 VBentry, 20 SOFTend, 21 cw/chain, 22 VBraise | VERTB/SOFTINT/task-switch timing | mixed — partly CPU |
| **E. Copper-vs-CPU phase** | 27 | copper WAIT-release + INTREQ visibility timing | **chipset-intrinsic — do-able now** |
| **F. Blitter-vs-beam** | 23 clr, 24 fill, 25 line, 26 fill+3bpl | blitter per-word cost (23/24), line-mode cadence (25), display-DMA-vs-blitter contention (26) | **chipset-intrinsic — do-able now** |

Chipset-intrinsic and actionable **without** the CPU adapter: **E and F**. Everything
else clusters with the deferred CPU-adapter work (families A/B/C, most of D), because
it needs the CPU to honor bus stalls / a faithful cycle ratio.

## Slice plan & validation (harness timing-test vs FS-UAE reference)

The chipset-intrinsic rows (23–27) are CPU-independent (the blit/copper runs
autonomously while the CPU polls), so the harness — even on Musashi+KS13 — is
comparable to the FS-UAE reference for these rows. Measured with the gate ON:

| Row | Test | OFF | ON | REF | ON/REF |
|---|---|---|---|---|---|
| 23 | D-only clear | 4974 | 9867 | 9908 | **1.00** ✅ |
| 24 | A→D fill | 6125 | 18256 | 18357 | **0.99** ✅ |
| 26 | fill + 3bpl (contention) | 8366 | 24934 | 25208 | **0.99** ✅ |
| 25 | line | 130 | 193 | 262 | **0.74** ⚠ (was 0.49) |

1. **Blitter per-word cost** (rows 23/24) — channel count + D-without-C idle cycle.
   *(done, commit 82977f6; validated 0.99–1.00.)*
2. **Row 26 contention** — CONFIRMED already structural (blitter steals only FREE
   slots; bitplane slots reduce them). Correct per-word cost lands it at 0.99 with
   **zero new code**. *(done — validation only.)*
3. **Row 25 line cadence** — two bus cycles/pixel (C read + D write). *(commit
   0459873; 0.49→0.74, residual tracked — do not fudge.)*
4. **Family E, row 27: copper-vs-CPU phase** — CONFIRMED already correct: harness
   row 27 = `0x640C`, exact match to the reference beam (vpos=$64, hpos=$0C). No
   code needed.

With rows 23/24/26 at parity, flipping the gate default is now gated only on the
boot oracle (`qemu_bisect_boot.sh`) — deferred until the user wants it enabled.

## Status of the chipset-intrinsic families (E, F)

Both are done: F (blitter) copy/fill/contention at 0.99–1.00 and line improved
0.49→0.74 (residual tracked); E (copper phase) already exact. Every other
divergence the timing-test exposes (families A/B/C, interrupt-latency rows 19/20/22
of D) sits in the deferred CPU-adapter cluster — the harness confirms rows 19/20/22
diverge (vpos/hpos off), consistent with CPU-coupled interrupt-recognition latency,
not a chipset bug. Chipset-side timing work for this issue is complete pending the
line residual and the eventual default flip.

Deferred cluster (moves with the CPU adapter, tracked separately per user):

6. Families A/B/C + most of D — CPU cycle ratio, memory-access cost, DMA contention
   on the CPU (`cpu_would_stall`), interrupt latency. These need the CPU adapter to
   honor bus policy, so they wait for that focused effort.

## Guardrails

- **Opt-in only.** No cycle-exactness change ships enabled by default until proven
  against both oracles (Copperline timing-test + `qemu_bisect_boot.sh`). Slice 1's
  default builds are byte-identical (gate OFF).
- **One timeline.** Never introduce a `now` parallel to `chipset.cycles`. The "bus"
  is the arbitration rule over the existing scheduler, not a new time holder. The
  stub `rigel_bus.c` (`last_addr` only) must not become a clock.
- **GPL boundary.** Copperline is GPL-3.0-or-later; Rigel is not. Copperline is an
  **oracle and behaviour teacher only** (its timing is HRM hardware behaviour, not
  its IP). Never translate its Rust (`src/bus/*.rs`) into Rigel C. Learn the
  hardware sequence, reimplement independently, validate against the oracle.

## Deferred — CPU adapter bus policies (separate focused effort, per user)

Families A/B/C and most of D above wait for this. A 68000 and a 68040 need different
policies to talk to Rigel, but the difference is in the *adapter* (CPU↔bus relation),
not the chipset timebase: 68000 = 16-bit, no cache, per-access chip contention (4
CCK/access); 68040 = cache+burst+32-bit, sparse chip touches. Musashi can stall
per-access (memory callback); Emu68 JIT must stall statistically per quantum. To be
picked up as its own effort, not folded into the chipset slices.
