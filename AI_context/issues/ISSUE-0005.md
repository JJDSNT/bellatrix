---
id: ISSUE-0005
title: "ARM interrupt normalization (bellatrix_irqfabric)"
status: blocked
priority: low
type: feature
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - interrupts
  - arm
  - irq
  - usb
  - bluetooth
  - multicore
related_files:
  - src/machine/machine_rigel_step.c
  - src/host/raspi3/pal_ipl.c
  - src/runtime/mailbox.c
---

# Issue: ARM interrupt normalization (bellatrix_irqfabric)

## Context

`interrupcoes.md` (Phases 3-4) proposes a `bellatrix_irqfabric` module that
normalizes ARM-level interrupts (IRQ, IPI, and eventually FIQ) into Bellatrix
events that are only processed at safe checkpoints.

The audit in ISSUE-0004 confirmed that **this module is not
needed today** because all I/O (USB DWC2, BT PL011, timer) is polled, not
interrupt-driven. However, this issue records the design so that when native ARM
IRQ handlers are eventually added, they are implemented correctly from the start.

---

## Current state (polling model)

```
USB DWC2 events  ─┐
BT PL011 bytes   ─┤→  PAL_Runtime_Poll()  →  polled ~1/1024 bus faults
Timer ticks      ─┘                           or every Musashi loop iteration
```

`PAL_Runtime_Poll()` calls:
- `bt_host_step()` (BT state machine)
- `usb_host_step()` (USB HCD)
- No ARM IRQ involved

**Risk of the current approach:** poll rate is tied to MMIO frequency. If the
M68K is in a tight loop with few MMIO accesses (e.g., waiting on a blitter),
`PAL_Runtime_Poll()` is called infrequently and USB/BT input can be delayed.

---

## When this becomes necessary

When any of the following are added:
- ARM IRQ from USB DWC2 (interrupt-driven USB HCD instead of polling)
- ARM IRQ from PL011 UART (interrupt-driven BT byte reception)
- ARM timer IRQ for precise audio output cadence

At that point, the ARM IRQ handler **must not**:
- Read or write M68K state directly
- Call `PAL_IPL_Set()`
- Invoke any Bellatrix machine/chipset code
- Use any resource protected by `core_chipset_lock`

---

## Design (implement when first ARM IRQ is added)

### Proposed module: `src/machine/irq/bellatrix_irqfabric.h/.c`

```c
typedef enum {
    BELLATRIX_IRQ_SOURCE_USB     = 0x01u,
    BELLATRIX_IRQ_SOURCE_BT_UART = 0x02u,
    BELLATRIX_IRQ_SOURCE_TIMER   = 0x04u,
} BellatrixIrqSource;

void bellatrix_irqfabric_init(void);

/* Called ONLY from ARM IRQ handler — sets a flag, nothing else */
void bellatrix_irqfabric_notify_host_irq(BellatrixIrqSource source);

/* Called from safe checkpoint (quantum loop / PAL_Runtime_Poll) */
bool bellatrix_irqfabric_has_pending(void);
BellatrixIrqSource bellatrix_irqfabric_drain(void);
```

The ARM IRQ handler body must be:
```c
void arm_irq_handler_usb(void)
{
    bellatrix_irqfabric_notify_host_irq(BELLATRIX_IRQ_SOURCE_USB);
    /* acknowledge the peripheral IRQ at hardware level — nothing else */
    dwc2_clear_interrupt();
}
```

The pending flag is then drained by `PAL_Runtime_Poll()` at a safe point:

```c
void PAL_Runtime_Poll(void)
{
    BellatrixIrqSource pending = bellatrix_irqfabric_drain();
    if (pending & BELLATRIX_IRQ_SOURCE_USB)
        usb_host_step(...);
    if (pending & BELLATRIX_IRQ_SOURCE_BT_UART)
        bt_hal_raspi3_poll_uart(...);
    // ...
}
```

### IPI (inter-core) protocol

Multicore IPI already uses the ARM mailbox (`src/runtime/mailbox.c`). The
irqfabric does not need to replace this; it supplements it for peripheral IRQs
on Core 3 (IO core).

### FIQ

**Keep FIQ disabled.** The analysis in `interrupcoes.md` is correct: FIQ can
only be used safely if VBAR_EL1 FIQ vectors save all Emu68 fixed registers
(x13-x29 + x18 + v30) before doing anything. There is no confirmed performance
need that would justify this complexity. This decision revisit criterion:
- Profiling shows >5% of frame time spent in ARM IRQ entry/exit for a peripheral
- AND the peripheral latency directly affects observable emulation quality

---

## VBAR_EL1 safety summary (from Phase 0 audit)

| IRQ type | Status | Safety |
|----------|--------|--------|
| Data abort (MMIO) | Active | Safe — x18/v30 saved in `patches/0002` |
| ARM IRQ | Not installed | N/A today |
| FIQ | Not installed | Must save x13-x29, x18, v30 before use |
| IPI via mailbox | Active (multicore) | Safe — polled by Core 0 WFE loop |

---

## Status

- [ ] Not started — **blocked by: no ARM IRQ handlers exist yet**
- Prerequisite: a peripheral that needs interrupt-driven servicing
- No action needed until then
- When implementing: put `bellatrix_irqfabric_notify_host_irq()` in a dedicated
  `.c` file that can be linked into the bare-metal IRQ vector table without
  pulling in chipset/machine headers
