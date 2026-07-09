---
id: ISSUE-0007
title: "Multicore runtime — Core 0 arbiter, MMIO critical barrier, deadline scheduling"
status: doing
priority: high
type: feature
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - multicore
  - scheduler
  - arbiter
  - performance
  - core0
  - emu68
  - mmio
related_files:
  - src/runtime/core_cpu.c
  - src/runtime/core_chipset.c
  - src/cpu/cpu_bridge.c
  - src/machine/machine_rigel_step.c
  - src/cpu/emu68/bellatrix.c
---

# Issue: Multicore runtime — Core 0 arbiter, MMIO critical barrier, deadline scheduling

## Relação com outras issues

Emu68 (ISSUE-0002), IRQ/temporal window (ISSUE-0004, 0006) e esta issue são faces
da mesma frente: o modelo correto de como CPU/JIT, chipset e IO se sincronizam no
tempo. O Musashi também roda em multicore (Core 1 = CPU, Core 2 = Rigel) e tem
papel duplo: referência de comportamento correto, e o backend mais fácil para
desenvolver e validar o controle multicore. Com Musashi, acessos MMIO são
chamadas C normais — é possível inserir pontos de sincronização exatamente onde
é necessário. Com o JIT do Emu68, o único ponto de intervenção é o data-abort
AArch64, que ocorre fora do controle do caller. Por isso faz sentido implementar
e estabilizar o modelo multicore no Musashi primeiro, e só depois adaptar para
o caminho Emu68.

O profiling (ISSUE-0001) é o guardrail para não implementar às cegas, mas não é
pré-requisito: podemos avançar o design em paralelo.

## Checkpoint: multicore redesign (core relabeling) — DONE

Session 2026-06-18 implemented the `multicore.md` target core mapping:

```
Core 0 — Machine/Host: boot + bellatrix_init(), then parks in wfe (no recurring work)
Core 1 — CPU: Emu68 JIT or Musashi (whichever backend is selected)
Core 2 — Chipset (Rigel): single-thread, via rigel_step()
Core 3 — IO físico: USB + Bluetooth (unchanged)
```

What changed (see `AI_context/consolidated/issue_multicore_runtime.md` for the
full technical writeup):

* `emu68/src/aarch64/start.c` hands the CPU backend off to Core 1
  (`bellatrix_launch_cpu_and_park()`) instead of running it inline on the
  boot core forever.
* The dead "Core 2 Audio" skeleton (pre-Rigel leftover, never launched) was
  repurposed into the real Core 2 Chipset loop.
* Closed a real correctness gap: the chipset access lock
  (`core_chipset_lock_acquire/release()`, now in `src/runtime/core_chipset.c`)
  previously only guarded Emu68's fault path; Musashi called the bridge
  directly with no lock. Now both backends go through the same lock inside
  `cpu_bridge.c`.
* All four release configs build clean; the Musashi harness still boots
  Kickstart and passes its 4 tests (single-core path, unaffected by this
  change, used as the regression check).
* **Not yet validated**: real multicore boot on hardware (Core 1 actually
  executing the JIT, Core 2 draining chipset cycles under contention). Can't
  be exercised from this environment — needs flash + boot + serial log on a
  real Pi 3.

## Evaluation: was this positive?

Yes. Three independent reasons:

1. It fixed a latent bug (Musashi + multicore had no lock around bridge
   calls), not just relabeled things.
2. It closed the gap between what "multicore" claimed to do and what it
   actually did — CPU execution was never on its own core before this; only
   chipset+IO were offloaded.
3. It was low-risk to implement *because* the existing cycle-publish/lock
   protocol (atomics + WFE/SEV) was already core-agnostic — this was a
   handoff/relabeling change, not a synchronization rewrite. Confirmed by
   build-green across all 4 configs and an unchanged harness regression pass.

## Evaluation: is the Core 0 arbiter idea worth pursuing?

Plausible, but currently **speculative** — there is no hardware profiling
data yet showing the gaps it would close actually cause observable problems.
Two specific open items from `multicore.md`'s gap analysis would be its
job if implemented:

* **MMIO critical barrier** — today, `bellatrix_bridge_cpu_read/write()`
  mutually excludes Core 1 and Core 2 (the lock), but does *not* guarantee
  Core 2's `s_chipset_cck` has caught up to the CPU's logical "now" before a
  critical write (`DMACON`, `INTENA/INTREQ`, `COPJMP`, `BLTSIZE`,
  `DDFSTRT/STOP`) is applied, or before a critical read (`DMACONR`,
  `INTREQR`, `VHPOSR`, `DSKBYTR`) is satisfied. Core 2 drains asynchronously
  in `CHIPSET_QUANTUM=128` CCK blocks, so there's up to ~128 CCK of possible
  lag between "CPU's view of time" and "chipset's actual state" at the
  moment of a critical access.
* **Deadline-oriented scheduling** — today's scheduler is a fixed-quantum
  accumulator, not `T = min(next chipset event, next IRQ, next critical
  MMIO requirement, max horizon)` as `multicore.md` specifies.

### Design risk to flag now, before any implementation

There are two very different shapes this could take, and they have opposite
performance implications:

1. **Synchronous round-trip arbiter** — Core 1 enqueues every critical MMIO
   access to Core 0, which forces Core 2 to catch up, performs the
   read/write, and replies. This guarantees correctness but turns Core 0
   into a hot path on every critical access — directly contradicting
   `multicore.md`'s "Core 0 deve permanecer leve" principle, and likely
   *hurting* throughput rather than helping it.
2. **Published scheduling horizon** — Core 0 (or whoever holds this logic)
   computes and publishes `T` periodically; Core 1 and Core 2 self-enforce
   catch-up against it without a round trip through Core 0 for every access.
   Lower overhead, more consistent with the "stays light" goal, but harder
   to get right (needs careful definition of what "next critical MMIO
   requirement" means in a deadline formula).

(2) seems like the right default to design toward, but this is a judgment
call that should be revisited once there's real data (see below) — don't
commit to either shape without evidence the gap matters in practice.

## 2026-07-09: Musashi multicore profiling instrumentation started

Branch `wip/multicore-runtime` starts with the conservative path recommended
above: validate and measure the existing Musashi multicore runtime before
changing synchronization semantics.

Implemented instrumentation:

* `core_chipset_get_progress()` exposes a snapshot of Core 2's drained CCK and
  Core 1's published target CCK.
* `cpu_bridge.c` samples critical MMIO accesses under `BELLATRIX_PROFILE` before
  dispatch/lock, using the allow-list from this issue:
  `DMACON`, `INTENA/INTREQ`, `COPJMP1/2`, `BLTSIZE`, `DDFSTRT/DDFSTOP`,
  CIA, and critical reads including `DMACONR`, `INTENAR`, `INTREQR`,
  `VHPOSR`, `DSKBYTR`.
* `BellatrixMulticoreStats` now reports critical-MMIO read/write counts,
  sample count, caught-up count, average backlog, and max backlog.

Validation so far:

```bash
BELLATRIX_CPU_BACKEND=musashi \
BELLATRIX_MULTICORE_BUILD=1 \
BELLATRIX_PROFILE=1 \
BELLATRIX_HDMI_AUDIO=0 \
BELLATRIX_USBSTACK=1 \
BELLATRIX_BTSTACK=0 \
BELLATRIX_LAUNCHER=0 \
bash scripts/build.sh
```

Build passed and generated
`emu68/install-bellatrix-rigel-musashi/Emu68.img`.

QEMU rough sanity:

```bash
qemu-system-aarch64 -M raspi3b -accel tcg,tb-size=64 \
  -kernel emu68/install-bellatrix-rigel-musashi/Emu68.img \
  -dtb emu68/install-bellatrix-rigel-musashi/bcm2710-rpi-3-b.dtb \
  -serial null -serial stdio -display none \
  -append enable_cache -initrd src/roms/aros.rom
```

Observed boot reached:

```text
[BELA] Initialized (multicore enabled: Core1=CPU Core2=Chipset Core3=IO)
[BELA] CPU backend: musashi (68040)
[BELA] MMIO profiling: ENABLED (BELLATRIX_PROFILE=1)
```

The short QEMU run timed out by design after initialization. Treat this only as
a bootstrap sanity check, not proof of functional multicore timing. The next
useful run is a longer QEMU or real-Pi run that triggers `BPROF` dump/reset via
`0xDFFF04`.

## Next steps (evaluation, not implementation)

Before writing any arbiter/scheduler code:

1. **Get the missing profiling metrics** that `multicore.md`'s "O que o
   profiling deve provar" section already calls for and that don't exist
   yet: `mmio_catchup_time`, `mmio_catchup_wait`, `critical_mmio_count`,
   `critical_mmio_backlog`. Add these to `bellatrix_profile.c`.
2. **Run real workloads on hardware** (Kickstart boot, Workbench, at least
   one timing-sensitive demo/game) in multicore mode and check whether the
   critical-MMIO lag is ever non-zero in practice, and whether it correlates
   with any observable glitch (corrupted copper lists, missed blitter
   finish, audio glitches, etc.).
3. **Only if step 2 shows a real problem**, design the actual mechanism
   (horizon-publish vs round-trip arbiter) as a separate plan, informed by
   the measured backlog sizes — that determines whether a cheap
   self-enforced catch-up is enough or whether true arbitration is needed.
4. If step 2 shows *no* observable problem, leave Core 0 parked. An idle
   `wfe` core costs nothing and is not technical debt — it's fine to leave
   this as "designed for, not built" until there's evidence it's needed.

## Related

* `AI_context/consolidated/issue_multicore_runtime.md` — current
  architecture writeup (cycle flow, lock, boot sequence).
* ISSUE-0002 — earlier performance analysis that first proposed the MMIO
  flush-policy distinction this barrier work would build on.
