# ISSUE-0072 — CPU adapters: CpuBackend + CpuBusPolicy (68000 / 68040 / Emu68)

**Status:** doing — CpuBusPolicy wired for Musashi; 68000 chip-bus wait active.
**Area:** Bellatrix CPU integration (`src/cpu/`, `src/machine/machine_rigel_bus.c`).
**Relates to:** ISSUE-0071 (chip-bus timing — the chipset half), the deferred
CPU-coupled timing-test families A/B/C/D, [[bellatrix-vertb-idle-loop-2026-07-16]]
(two IPL-delivery paths), [[bellatrix-battle-squadron-state]].

## The adapter already exists — it is `CpuBackend`

`src/cpu/cpu_backend.h` is described in-tree as "the machine's only dependency on
the CPU." It already provides the asymmetric execution primitive the design needs:

```c
typedef struct CpuBackend {
    void *ctx;
    uint32_t (*get_pc)(void *ctx);
    void     (*set_ipl)(void *ctx, int level);   /* IRQ publication */
    void     (*reset)(void *ctx);
    int      (*run)(void *ctx, uint32_t cycles);  /* each backend runs its own way */
    int      (*map_direct)(...); int (*unmap_direct)(...);
    int      progress_in_run;
} CpuBackend;
/* cpu_backend_selected() / _init_selected() — generic layer drives Emu68 or Musashi. */
```

Both backends already funnel their bus accesses through the shared seam
`machine_rigel_bus.c` (`machine_dispatch_read/write`, `bellatrix_machine_read/write`),
which already decomposes a 32-bit access into two 16-bit chip-bus transfers. Rigel
is already CPU-agnostic. So most of the proposed architecture is built; what is
missing is **per-CPU bus policy**.

`run(ctx, cycles)` is the confirmation that the **asymmetric-execution interface**
(option a) is the right one: Musashi's run = `m68k_execute(cycles)` (interpreter,
can consult `cpu_would_stall` per access); Emu68's run = its JIT quantum loop
(faults into `bellatrix_bus_access`, reconciles stall in aggregate). We do NOT force
Emu68 into a per-access pull model.

## What's missing: CpuBusPolicy (the per-CPU deltas)

Today the CPU model is a compile-time `#define BELLATRIX_MUSASHI_CPU_MODEL =
68040` and there is no representation of the CPU→bus relationship. Add a small,
backend-agnostic policy carrying the axes the timing-test measures:

| Axis | Timing-test family | Policy field |
|---|---|---|
| CPU model (removes the #define) | — | `model` |
| cycle → Rigel-time ratio | A (move/shift/mul/dbra) | `cpu_clock_hz` (Musashi models per-insn cycles; the clock sets cycles→CCK) |
| stall on chip access | C (cw/Nbpl) | `stalls_on_chip_access` (gate for consulting `cpu_would_stall`) |
| IRQ recognition latency | D (VBentry/…) | `irq_recognition_latency` |

Note (grounded in the code): the chip bus is 16-bit for **both** 68000 and 68040
(Agnus never widened), so access decomposition to the chip bus is CPU-independent —
it is NOT a policy axis. The 040's 32-bit/burst bus only matters for Fast RAM, which
bypasses Rigel. Do not model access-width differences for the chip bus.

## Boundaries (who owns what)

- **Rigel** — CPU-agnostic time/bus authority. Unchanged. Exposes `cpu_would_stall`,
  `rigel_get_next_deadline`, `rigel_step_until`, `rigel_custom_*`.
- **CpuBackend** — the shared execution shell (init/run/reset/pc/ipl/direct-map).
  One per engine: Musashi, Emu68. Emu68 is effectively a ~68040.
- **CpuBusPolicy** — shared data describing a CPU model's bus behaviour. One Musashi
  backend + {68000, 68040} policies = the two "configs of one backend." Lives once
  (`src/cpu/cpu_bus_policy.*`), consumed by any backend.
- **machine_rigel_bus.c** — the transaction seam. Stays shared; grows a policy-aware
  stall hook (family C) later.

## Incremental plan (each step byte-identical until proven)

1. **Contract** — `CpuBusPolicy` + `cpu_bus_policy_68000/_68040` as data. *(this step,
   unwired.)*
2. **Runtime model** — Musashi derives its m68k type from the selected policy instead
   of the `#define`. Default 68040 → byte-identical.
3. **Family A (ratio)** — drive cycles→Rigel-time from `cpu_clock_hz`. Validate the
   move/shift/mul/dbra rows per policy.
4. **Family C (stall)** — wire `cpu_would_stall` in the Musashi memory callback under
   `stalls_on_chip_access`. Validate cw/Nbpl rows.
5. **Family D (IRQ latency)** — apply `irq_recognition_latency`. Validate VBentry rows.
6. Emu68: aggregate/quantum reconciliation of the same policy where observable.

## 2026-07-24 — first wired slice: Musashi 68000 consumes Rigel stalls

The policy is now compiled into both product and harness. The Musashi adapter
selects it from the actual CPU model, publishes in-timeslice CPU progress at
memory callbacks, and the 68000 policy consults Rigel before Chip RAM accesses.
The 68040 policy remains non-stalling.

This exposed a real latent Rigel API defect: while the current slot was occupied,
`rigel_bus_state.next_change` used `agnus_slot_scheduler_next_event()`, which
finds the next *occupied* slot. A waiting CPU needs the first CPU/free slot.
Rigel now provides `agnus_slot_scheduler_cpu_resume_in()` and uses it for the
stalled branch. A focused `test_agnus_domains` case pins an occupied-occupied-free
sequence to a resume delta of 2 CCK.

The hot path was also split: `rigel_cpu_would_stall()` answers the common
yes/no question in O(1); only an actual stall computes `next_change`. Calling
the full `rigel_get_bus_state()` at every Musashi fetch scanned up to a line of
slots and made the timing-test impractically slow.

Validation completed:

- `bellatrix_unit_cpu_bus_policy`: pass;
- `test_agnus_domains`: pass, including first-free-slot resume;
- harness build: pass;
- real `BELLATRIX_RELEASE_PROFILE=musashi_68000 ./scripts/build.sh`: pass,
  image at `emu68/install-bellatrix-rigel-musashi/Emu68.img`.

The product build uncovered an unrelated existing launcher declaration defect
when USB is disabled: `media_selection.c` called
`bellatrix_runtime_io_pump()` without including `runtime/runtime.h`. The include
is now unconditional and the bare-metal build completes.

The full 32-row 68000 timing-test is not yet accepted: with per-access
arbitration enabled, the active-display part remains too slow to finish inside
the former 90-second host timeout. Do not update a golden from a partial run.
Next work is to profile/count stall callbacks and batch the Rigel-only wait path
without weakening slot ordering, then capture and compare all families B/C.

Emu68/multicore remains deliberately unwired: its policy is 68040 and requires
quantum-level stall reconciliation on the Rigel-owner core, not synchronous
per-fault stepping from the CPU core.

## 2026-07-24 — full 68000 transaction and corrected DMA ownership

The initial boolean wait was insufficient: Musashi reports many memory callbacks
before charging the enclosing instruction, so sampling only the current owner
let multiple accesses reuse one apparent free instant. The adapter now executes
an explicit OCS/ECS transaction:

1. publish CPU progress up to the callback;
2. wait for the first CPU-accessible slot;
3. advance the complete four-clock / two-CCK 68000 word cycle;
4. split a long access into two word transfers;
5. mark the nominal four Musashi clocks per word as already published, avoiding
   double charging when Musashi retires the instruction;
6. charge arbitration waits as additional Musashi clocks with
   `m68k_modify_timeslice()`.

That work exposed a second latent producer defect. `copper_active` meant only
that COPEN was enabled, but the scheduler treated it as a request on every FREE
slot. A Copper sleeping in WAIT therefore starved the CPU indefinitely. Rigel
now advances the WAIT comparator every CCK without owning the bus and asserts
`copper_request` only when `fetch_pending` needs an instruction fetch. The
focused scheduler test covers sleeping-vs-requesting Copper.

The lores bitplane calendar was also wrong: it packed N planes into N adjacent
slots. OCS/ECS hardware uses the eight-CCK order `8,4,6,2,7,3,5,1`; a six-plane
group leaves offsets 0 and 4 free. Correcting that order changed the timing-test
contention result from an over-stalled ~1.90x to:

| row | scenario | result |
|---:|---|---:|
| 10 | no display DMA | `0x073A` |
| 11 | 6-plane lores | `0x09FB` (1.37x row 10) |
| 12 | 8 sprites | `0x073A` |
| 15 | 6 planes + sprites | `0x09FB` |
| 18 | 3-plane lores | `0x0739` |

Copperline's in-tree audit for the equivalent `move.w + dbra` loop documents
approximately 9.08 CCK/iteration without DMA and 13.15 with six planes (1.45x).
Bellatrix is now in the same range; the remaining difference and the zero
three-plane/sprite deltas still need a matched 68000 capture before acceptance.
Cargo is not installed in the current environment, so the Copperline executable
could not be rebuilt locally.

The complete 32-row harness run now finishes again in roughly 7.4 seconds
(formerly it timed out while the sleeping Copper falsely owned every FREE
slot). The real Musashi 68000 bare-metal profile builds successfully with the
transaction enabled. Hardware FPS remains to be measured; do not infer Pi 3B
performance from the hosted runtime.

## Prerequisite for validation

The reference we have is A500+ **68EC020**. To validate a 68000 policy we need a
68000 golden capture; for 68040, a 68040 capture (the timing-test README ships
A1200/020 configs to model from). Matched references per policy are a prerequisite,
not a detail — otherwise family A/C/D rows are not apples-to-apples.

## Convergence note

Product (`src/cpu/musashi/`) and harness (`tools/harness/musashi_backend.c`)
retain different memory maps and diagnostics, but their temporal integration
now converges in `src/cpu/musashi/musashi_bus_timing.c`. Progress publication,
function-code tracking, cycle compensation, explicit transactions and slot
waits are the same compiled code in both.

## 2026-07-24 — 68EC020 policy and matched-model baseline

Musashi already supported `M68K_CPU_TYPE_68EC020`, but the harness reduced every
non-68000 model to `cpu_bus_policy_68040`. A distinct `68ec020` policy and
`musashi_68ec020` release profile now select the real model in both product and
harness. Function-code emulation is enabled so the shared adapter can
distinguish program prefetch from data.

The FS-UAE A500+/68EC020 reference rejected applying the 68000 transaction to
every callback: `cw1024` became about 1.45x slow without DMA and 2.25x slow with
six planes. The retained initial model accounts chip-resident program fetches
without synchronously waiting them on DMA. Shift, DBRA, DBRA in slow/chip RAM,
frame, blitter clear/fill and fill+3bpl are near the reference. Move/multiply
and several memory rows still diverge; do not add per-opcode fudge factors
without a matched physical capture.

Validation:

- shared harness build: pass;
- Bellatrix unit suite plus harness smoke (10 tests): pass;
- 68000 rows 10/11/12/15/18 remain `073A/09FB/073A/09FB/0739`;
- `BELLATRIX_RELEASE_PROFILE=musashi_68ec020` cross-build: pass;
- QEMU KS1.3 smoke: pass, frames 1→10 at 3–4% TCG realtime.

## Battle Squadron — attribution, not a fix

The known Battle Squadron corruption is a chipset-side blitter seam (starfield tiles),
and it runs the CPU as 68000. Adapters do not "fix" it; their value there is
**attribution** — separating backend vs CPU-model vs Rigel when Musashi and Emu68
disagree. Do not expect the adapter work to resolve that bug.
