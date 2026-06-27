---
id: ISSUE-0006
title: "Temporal window truncation for critical MMIO writes"
status: backlog
priority: medium
type: feature
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - timing
  - mmio
  - emu68
  - copper
  - blitter
  - quantum
related_files:
  - src/machine/machine.h
  - src/machine/machine_rigel_step.c
  - src/machine/machine_rigel_bus.c
---

# Issue: Temporal window truncation for critical MMIO writes

## Context

`interrupcoes.md` (Phases 1-2-7) proposes formalizing the "quantum window" as a
first-class concept and truncating it early when the CPU writes to registers that
change observable chipset behavior immediately.

The audit in ISSUE-0004 confirmed:
- `bellatrix_machine_recommended_cpu_quantum()` already queries Rigel for the
  next CCK deadline and the next bus-ownership change.
- After any MMIO write, `bellatrix_machine_sync_ipl()` is called to propagate
  IPL — but the Emu68 JIT has already returned from the fault handler before
  this call, so a few more instructions execute before the JIT sees the new IPL.
- There is no mechanism to signal "this write requires exiting the window now."

---

## Problem

When the CPU writes to a critical register the chipset must act on immediately,
the current flow is:

```
JIT executes MMIO write
  → fault handler → bellatrix_bus_access() → machine_write()
  → sync_ipl() sets INT.IPL
  → fault handler returns
  → JIT continues executing (IPL not checked until next loop iteration)
  → next loop iteration top: JIT reads INT.IPL → takes interrupt
```

The gap between "IPL set" and "JIT checks IPL" is one JIT block (~1-64
instructions depending on block size). For most cases this is harmless, but for
writes to INTENA that unmask an already-pending INTREQ, or a COPJMP that the
Copper must start immediately, the delay is incorrect.

**This is not a catastrophic bug today** because AROS/Kickstart generally does
not rely on sub-instruction interrupt latency. But it will matter for:
- Copper timing (COPJMP should be visible at the cycle it is written)
- Blitter-done interrupt (BLTSIZE write must be followed immediately by blitter
  busy state)
- Audio DMA start (correct Paula behavior depends on exact cycle)

---

## Design

### What already exists

`bellatrix_machine_recommended_cpu_quantum()` in `machine_rigel_step.c` returns
the number of CPU cycles until the next Rigel deadline. This is already the
temporal window — it just isn't used as a hard exit condition.

`RuntimeSync` in `src/runtime/sync.c` is a barrier synchronizer (cpu_ready,
gfx_ready flags) — it does **not** manage temporal windows.

### What to add

#### 1. Window exit flag (minimal, no new module needed)

Add a single atomic flag that the MMIO write path can set to request early exit:

```c
// src/machine/machine_rigel_internal.h (or machine.h)
void bellatrix_machine_request_window_exit(void);
bool bellatrix_machine_should_exit_window(void);
void bellatrix_machine_clear_window_exit(void);
```

The flag lives in `BellatrixMachine` or as a file-static in
`machine_rigel_step.c`. The quantum loop in `machine_rigel_step.c` checks it
each iteration.

#### 2. Critical register list in `machine_rigel_bus.c`

In `machine_dispatch_write()` (called from `bellatrix_machine_write()`), after
writing the register, check if the address is in the critical set and call
`bellatrix_machine_request_window_exit()`:

```c
// Critical registers: truncate the quantum window on write
static bool is_window_truncating_write(uint32_t addr)
{
    uint32_t reg = addr & 0x1FFu;
    switch (reg) {
    case 0x096u: /* DMACON  */
    case 0x09Au: /* INTENA  */
    case 0x09Cu: /* INTREQ  */
    case 0x088u: /* COPJMP1 */
    case 0x08Au: /* COPJMP2 */
    case 0x080u: /* COP1LCH */
    case 0x082u: /* COP1LCL */
    case 0x058u: /* BLTSIZE */
    case 0x040u: /* BLTCON0 */
        return true;
    default:
        return false;
    }
}
```

#### 3. Quantum loop polls the flag

In `machine_rigel_step.c`, the inner loop that accumulates CPU cycles:

```c
// existing loop (simplified):
while (partial < s_quantum) {
    uint32_t delta = notify_cpu_progress(...);
    partial += delta;
    if (bellatrix_machine_should_exit_window()) {
        bellatrix_machine_clear_window_exit();
        break;  // truncate: advance Rigel to current partial, not full quantum
    }
}
```

This is safe because `notify_cpu_progress` is already the callback from the
Emu68 JIT that fires after each block.

#### 4. IPL re-check after truncation

After the truncated advance, call `bellatrix_machine_sync_ipl()` so the new IPL
value is in `INT.IPL` before the JIT continues. The JIT will see it on its next
instruction-boundary check.

---

## Implementation plan

| Step | File | Change | Size |
|------|------|--------|------|
| 1 | `src/machine/machine.h` | Add `uint8_t exit_window_requested` to `BellatrixMachine` | tiny |
| 2 | `src/machine/machine_rigel_step.c` | Add `request/should/clear_window_exit()` + poll in quantum loop | small |
| 3 | `src/machine/machine_rigel_bus.c` | Call `request_window_exit()` for critical registers after write | small |
| 4 | Validate | Run harness: verify VBL IRQ fires at correct beam line | test |

**Do not implement** `bellatrix_sync` as a separate module yet — the flag in
`BellatrixMachine` is sufficient and avoids premature abstraction. Revisit after
profiling confirms the latency matters in practice.

---

## Relation to ISSUE-0005 (`bellatrix_irqfabric`)

The irqfabric is about ARM-level interrupt normalization, which is a separate
concern. It is only needed when ARM IRQ handlers are added for USB/BT. Today
everything is polled.

## Relation to ISSUE-0003 (`emu68_host_ops`)

The `should_exit_window` callback in `emu68_host_ops` (from `interrupcoes.md`)
is a superset of this: it would let Emu68 query Bellatrix directly from inside
the JIT loop rather than via a flag polled by the chipset step. That requires
ISSUE-0003 to land first. This issue implements the same semantic at a lower
cost using the existing quantum loop.

---

## Status

- [ ] Not started
- Prerequisite: ISSUE-0004 (done — Phase 0 complete)
- Blocks: correct Copper/Blitter/audio DMA timing in Phase 4-5 of the project roadmap
