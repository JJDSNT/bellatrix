---
title: "Replace the inherited AArch64 DWC2 port with a Bellatrix-owned driver"
status: in-progress
date: 2026-08-21
components:
  - usb
  - dwc2
  - poseidon
  - m68k-emu68
---

# Decision

Bellatrix will implement its own DWC2 host-controller driver under
`aros/arch/m68k-emu68/soc/usb/dwc2emu68`. The replacement now enumerates devices
and passes the QEMU smoke tests, so `usb2otg.device` and the patches which made
the arm-native implementation usable on m68k have been removed from the pack.

Only after the replacement works will redundant arm-native USB patches be
removed. Poseidon and the generic AROS USB stack are not being replaced.

# Why

The inherited driver intertwines the Poseidon ABI, controller scheduling,
platform DMA/cache rules, recovery and substantial work in interrupt context.
Repeated fixes did not remove the permanent 1 kHz SOF interrupt cost and have
made correctness difficult to reason about. The Bellatrix driver instead uses:

- a unit task as the sole owner of request and controller state;
- a minimal interrupt top half which only acknowledges/masks and signals;
- an explicit emu68 MMIO, DMA alias, cache and barrier boundary;
- event-driven SOF enabling rather than an unconditional 1 kHz guest IRQ;
- bounded task-context timeouts and recovery.

The emu68 xHCI driver is an architectural reference for ownership and interrupt
separation, not a hardware implementation source. The controller remains the
Pi 3/QEMU Synopsys DWC2.

# Final implementation

The `dwc2emu68.device` module is the distribution's active Poseidon host
controller, but end-to-end input remains under investigation. It:

- implements Open, Close, BeginIO, AbortIO and `NSCMD_DEVICEQUERY` entry points;
- sends requests to one unit task and never processes them in the caller;
- probes `KATTR_PeripheralBase + 0x00980000` for an OT2 `GSNPSID`;
- encapsulates endian-safe MMIO and m68k barriers in `dwc2emu68_platform.c`;
- initializes the DWC2 core in host mode with DMA and SOF masked;
- exposes a virtual root hub and implements downstream control transfers;
- acknowledges channel IRQ status in the hardware top half and defers the
  transfer state machine through an Exec software interrupt to the unit task;
- serializes concurrent Poseidon requests through a FIFO;
- polls interrupt-IN endpoints round-robin at 10 ms without a permanent SOF
  interrupt, and lowers the worker priority to avoid starving the desktop.

Validated with:

```text
make kernel-usb-dwc2emu68
./scripts/setup.sh --verify
make AROS-emu68-m68k
./scripts/make-sdcard.sh
./run.sh --headless --serial out/dwc2-qemu-final.log --sd out/aros/sd.img -- -device usb-tablet
```

The image reaches Wanderer and a QEMU `mouse_button` injection produces a
six-byte interrupt report on endpoint 1 (`01 00 00 00 00 00`). This proves the
controller-to-HID-buffer path, not visible pointer operation. The user reports
that the driver is not functional, so the issue must not be considered resolved
until the exact visible failure is reproduced and verified end to end.

`AbortIO()` was initially a stub. Poseidon can abort and replace pipes during
class selection, leaving an obsolete request active and completing into an
abandoned pipe. Cancellation of active, queued and root-hub requests is now
implemented and awaits end-to-end validation.

# Removed legacy patches

The obsolete arm-native compatibility series `0013`, `0016`, and `0018` through
`0022` was reverted and deleted after the integrated validation. Patch `0014`
remains as the generic Poseidon startup integration and patch `0053` selects the
Bellatrix-owned module.
