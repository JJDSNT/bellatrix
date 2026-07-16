# IRQs: physical ARM interrupts vs. emulated Amiga IPL

## Status

Core mechanism implemented and exercised in QEMU/emulation; hardware
validation of the physical-IRQ path is pending explicit authorization (see
`AI_context/issues/ISSUE-0058.md`). Treat this document as describing the
current implementation and its still-open edges, not a finished contract.

## Two contracts that must never be conflated

Bellatrix has two entirely separate interrupt concepts that happen to share
vocabulary ("IRQ") and, on the Emu68 side, even a little bit of storage:

| | What it is | Who produces it | Who consumes it |
| --- | --- | --- | --- |
| **Physical ARM IRQ/FIQ** | A BCM2837 hardware interrupt (GIC-less BCM2837 interrupt controller) | Real host peripherals (PL011/Bluetooth today; USB/DWC2 potentially in future) | The ARM core's own exception vector, top-half code |
| **Emulated Amiga IPL** | The 68k CPU's interrupt priority level (0-7), architectural Amiga state | Paula, after consolidating INTREQ/INTENA from CIA/Agnus events (see `runtime_and_timing.md` §8) | The 68k CPU core (Emu68's `MainLoop`, or Musashi's backend boundary) |

A physical ARM IRQ arriving does **not** automatically become an Amiga IPL.
Conflating the two — treating every physical interrupt as something the
guest 68k program must observe — was an early misreading of the PiStorm
design that `AI_context/issues/ISSUE-0058.md` explicitly corrected in
2026-07-15, after clarification from Emu68's author: Emu68 has no interrupt
service routine of its own. What PiStorm calls the "housekeeper" is a
polling loop on an auxiliary core that watches the *physical Amiga's* IPL
pins and forwards changes into `M68KState.INT.IPL` — it is not an ARM ISR,
and there is no equivalent physical-Amiga-bus signal in Bellatrix at all,
since Rigel *is* the chipset, not a device behind a physical bus.

Bellatrix's equivalent of the housekeeper is `PAL_IPL_Set()`
(`src/host/raspi3/pal_ipl.c`): whenever Rigel/Paula (running on Core 2)
derives a new persistent IPL level, `core_chipset.c` calls
`PAL_IPL_Set(ipl)`, which writes `M68KState.INT.IPL` directly and issues an
`SEV` so Core 0 (currently the Emu68/JIT-owning core) wakes from `WFE` if it
was parked in STOP. This is a **level publication**, not a pulse — it
carries no notion of "an interrupt just happened," only "the guest's
interrupt mask should currently see this level." A genuinely new,
ARM-side service that needs to *notify* AmigaOS of something must be its own
explicit, separately-designed channel — it must not silently reuse this one
or the physical-IRQ path below.

## The physical ARM IRQ/FIQ path

`emu68/src/aarch64/vectors.c` (patched by `patches/0022` and `patches/0023`)
implements the actual ARM exception vector table. Each slot is a fixed
**0x80-byte** architectural budget — this is not a stylistic constraint, it
is enforced by the ARMv8 vector table layout, and a handler that overflows
it corrupts execution by falling into the next slot's code. An earlier
Bluetooth-IRQ branch shipped exactly that bug; `scripts/check_bt_irq_abi.sh`
now gates every build by checking the compiled ELF's vector symbols land at
their architectural offsets (`+0x000` sync, `+0x080` IRQ, `+0x100` FIQ,
`+0x180` SError, repeated for SP0 and SPx).

Inside the 0x80-byte SPx IRQ slot, only `x0`/`x1` are saved before Bellatrix
gets a chance to look at the interrupt. The Bellatrix addition
(`bellatrix_spx_bt_irq` / `bellatrix_spx_unknown_irq` in `vectors.c`) reads
`IRQ_PENDING_2` right there and makes one decision, entirely within budget:

```text
GPU IRQ 57 (UART0 / Bluetooth)?
  yes -> branch to an out-of-line trampoline (outside the vector table,
         no 0x80 limit) that saves x2-x30 and all of q0-q31/FPCR/FPSR,
         calls bellatrix_physical_irq_handler(), restores everything, eret.
         Never touches INT.ARM. Never runs BTStack in the exception.
  no  -> fall through to Emu68's original path: publish INT_shadow /
         INT.ARM = level 6 (EXTER), exactly as PiStorm's physical-bus
         IRQ delivery always has.
```

`bellatrix_physical_irq_handler()` (`src/host/raspi3/physical_interrupts.c`)
is the entire top half for the Bluetooth source: it disables the UART0 IRQ
route at the BCM2837 controller (level-sensitive, so it must be masked
before the FIFO is touched), counts the event, and calls into
`bt_hal_raspi3_irq_rx()`, which drains PL011 into a software ring buffer —
no BTStack call, no allocator, no logger inside the exception. The Core 3
host reactor (see `host_reactor.md`) later drains that ring and re-arms the
route via `bellatrix_physical_bt_irq_rearm()`. Any GPU IRQ source other than
UART0 is counted as `unknown_irq_count` and masked off without ever being
converted into a guest-visible interrupt — `bellatrix_physical_unknown_irq_count()`
being non-zero in telemetry means a new physical source fired without
having an owner assigned yet, which is a configuration bug, not expected
behavior.

FIQ is architecturally reserved and currently has no enabled source. USB
DWC2's SOF handling is the one candidate mentioned for a future FIQ
consumer, precisely because it's more latency-sensitive than the Amiga
chipset's ~540 ns MMIO cycle; nothing should claim FIQ before that need is
measured and demonstrated.

## Why Bluetooth uses IRQ, not FIQ

Emulating the Amiga chipset itself has no need for a fast interrupt: a
~540 ns access cycle is not a latency-critical ARM peripheral by any
measure. FIQ is kept free for something that genuinely needs it later (see
above). Bluetooth (and any future non-Amiga host peripheral) uses a normal
ARM IRQ.

## Open edges

- Hardware validation of the physical-IRQ path (context-corruption absence,
  no spurious guest IPL, no dropped UART byte, no starvation under load) is
  explicitly deferred pending user authorization — see ISSUE-0058 P3.
- The exact physical producer of GPU IRQ 57 routing predates this
  architecture and was traced historically (commit `6275529` introduced the
  generic AArch64 async-interrupt field that became `INT.ARM`, `37dd583`
  added the `INTENA`/`INTREQ` shadow that presents it as `EXTER`); this
  channel is Emu68's own, not something Bellatrix invented, and Bellatrix's
  job is only to decide, per physical source, whether it belongs on this
  channel at all (Bluetooth: no) or needs a new explicit one (not yet
  designed for anything else).
- See [`fault_handler.md`](fault_handler.md) for the separate, and equally
  easy to conflate, distinction between MMIO routing and chipset-clock
  synchronization — the same "don't let one flag decide two questions" rule
  applies to IRQ work as much as it did to the fault-handler regression.
