---

> **Correção arquitetural posterior (2026-07-15):** esta auditoria assumia a
> topologia Bellatrix já patchada e tratava a ausência atual de IRQ como prova
> suficiente de independência. ISSUE-0058 exige comparação com o `HEAD` original
> e rebaseline do Emu68 no Core 0 antes de habilitar Bluetooth IRQ.
id: ISSUE-0004
title: "Interrupt model audit — Phase 0"
status: superseded
priority: medium
type: research
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - interrupts
  - emu68
  - ipl
  - audit
  - fiq
related_files:
  - src/host/raspi3/pal_ipl.c
  - src/machine/machine_rigel_bus.c
  - src/machine/machine_rigel_trace.c
  - src/machine/machine_rigel_step.c
  - src/cpu/emu68/bellatrix.c
  - src/runtime/core_cpu.c
  - patches/0002-add-bellatrix-bus-hook.patch
  - patches/0003-bellatrix-execution-loop.patch
---

# Issue: Interrupt model audit — Phase 0 verified reality

## Context

`interrupcoes.md` (session notes) proposed a 10-phase plan for making interrupt
handling safe between Emu68 and Bellatrix. This issue documents what Phase 0
(the audit) found by reading the actual code.

---

## What exists and is safe today

### ARM-level interrupt vectors

- **VBAR_EL1 is fully owned by Emu68.** Bellatrix installs no ARM IRQ, FIQ, or
  IPI handlers of its own. The only ARM exception path Bellatrix uses is the
  AArch64 data-abort fault, which is the existing MMIO mechanism
  (`SYSWriteValToAddr` / `SYSReadValFromAddr` in `patches/0002`).
- **FIQ is not used.** There is no FIQ handler and no FIQ-routed peripheral.
- **The concern in `interrupcoes.md` ("FIQ/IRQ invades JIT register state") is
  not a present risk.** It would only become a risk if Bellatrix tried to
  install a native ARM IRQ/FIQ handler.

### IPL injection path

```
Rigel emits RIGEL_EVENT_IRQ_CHANGED
  → machine_rigel_step.c: machine_publish_ipl()
  → machine_rigel_trace.c: machine->current_ipl = ipl; cpu_backend->set_ipl()
  → bellatrix.c: emu68_set_ipl() → PAL_IPL_Set()
  → pal_ipl.c: ctx->INT.IPL = level; DMB; SEV
```

`PAL_IPL_Set()` writes a single field in `M68KState`. The Emu68 JIT only reads
`INT.IPL` at the **top of its main while(1) loop** (patch `0003`), which is an
instruction-boundary checkpoint. This satisfies the key invariant from the doc:
*"Emu68 só deve observar interrupções M68K em pontos seguros."*

**The SEV is safe:** it wakes the core from WFE (STOP instruction) but does not
preempt the JIT mid-execution. The JIT continues to the next loop iteration,
then checks IPL.

### Additional sync points

- After every MMIO write: `machine_rigel_bus.c:490` calls
  `bellatrix_machine_sync_ipl()` → `machine_publish_ipl()` → `set_ipl()`.
- After every `machine_step_components()` tick: `RIGEL_EVENT_IRQ_CHANGED` fires.
- Musashi: `core_cpu_step()` calls `core_cpu_publish_interrupts()` before each
  run, reading `machine->current_ipl`.

### Registers the JIT cannot have clobbered

`patches/0002` (vectors.c) documents that the bus-fault path saves and restores
`x18` (M68K PC) and `v30` (JIT instruction counter) around any `kprintf` or
Bellatrix call. This is already required because `kprintf` is a C function that
can clobber caller-saved registers. The save/restore is implemented correctly.

Emu68's fixed register layout: **x13–x29** hold M68K state, **x18** is M68K PC,
**x12** is a JIT temp. Bellatrix code must never use these in any path that runs
without saving them — currently only the bus-fault path runs in that context and
it already saves/restores correctly.

---

## What is missing

### 1. No `bellatrix_irqfabric` (Phase 3 of the doc)

ARM-level I/O events (USB DWC2, BT UART PL011) are currently **polled**, not
interrupt-driven. `PAL_Runtime_Poll()` is called from:
- `bellatrix_runtime_poll_from_emu68()` (throttled ~1/1024 bus faults, single-core only)
- The Musashi loop in `bellatrix_run_selected_cpu_backend()` (every iteration)

There is no ARM IRQ handler for USB or BT. This is safe today but means:
- USB/BT latency depends on poll rate, not hardware timing.
- If a future change installs an ARM IRQ handler for a peripheral, there is no
  infrastructure to safely normalize it into a Bellatrix event.

**Risk:** Low today. The risk grows if any ARM IRQ is added without the
irqfabric pattern. See ISSUE-0005.

### 2. The `bellatrix_bridge_cpu_sync_ipl()` call is unreachable

`bellatrix.c:288` has `update_ipl()` calling `bellatrix_bridge_cpu_sync_ipl()`,
but `update_ipl()` is declared `__attribute__((unused))`. This dead code
suggests an intended periodic IPL re-sync from the bus-fault path that was
never wired up. Not a bug (IPL is already synced via `machine_publish_ipl`), but
a maintenance hazard.

### 3. IPL sync after MMIO write is post-hoc for Emu68

When `machine_rigel_bus.c:490` calls `sync_ipl()` after a write, the Emu68 JIT
has already returned from the fault handler and is executing the next
instruction. The IPL change is not visible until the **next loop iteration**
(next time the JIT checks `INT.IPL`). For most cases this is fine, but for a
write to INTENA that unmasks an already-pending INTREQ, the CPU could execute
a few instructions before taking the interrupt.

This is the same latency gap the doc describes in Phase 7 ("events that should
truncate the window"). See ISSUE-0006.

---

## Status

- [x] Phase 0 audit complete
- [ ] Phase 1 — formalize temporal window (see ISSUE-0006)
- [ ] Phase 3 — `bellatrix_irqfabric` (low priority while everything is polled, see ISSUE-0005)
- [ ] Fix dead `update_ipl()` code (cleanup, not correctness)
- [ ] Phases 5-6 — `emu68_host_ops` covered by ISSUE-0003

## Files audited

| File | Role |
|------|------|
| `src/host/raspi3/pal_ipl.c` | IPL injection into `M68KState.INT.IPL` |
| `patches/0003-bellatrix-execution-loop.patch` | JIT interrupt check at loop top |
| `patches/0002-add-bellatrix-bus-hook.patch` | x18/v30 save-restore in fault path |
| `src/machine/machine_rigel_trace.c:658` | `machine_publish_ipl()` |
| `src/machine/machine_rigel_bus.c:490` | Post-write IPL sync |
| `src/machine/machine_rigel_step.c:409` | `RIGEL_EVENT_IRQ_CHANGED` handler |
| `src/cpu/emu68/bellatrix.c` | `emu68_set_ipl()`, `update_ipl()` (dead) |
| `src/runtime/core_cpu.c` | Musashi IPL path |
