---
title: "Replace the inherited AArch64 DWC2 port with a Bellatrix-owned driver"
status: in-progress
updated_at: 2026-08-24
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

# State, corrected 2026-08-24

This section previously claimed the Bellatrix driver was the distribution's
active host controller. It was not. `mmakefile.src` listed the five inherited
`usb2otg_*` sources in `FILES` and renamed them at compile time with
`-Dusb2otg=dwc2emu68`; the `dwc2emu68_*` sources sat beside them and were
never compiled. The issue had run ahead of the code, and the mmakefile's own
comment said so.

## Two drivers, both ours, compared before either is deleted

The plan is now explicit, and it is the shape `soc/sdcard` already uses for
its two backends:

1. adopt the legacy engine as our own code, out of the patch series -- done;
2. port it by rewriting, gradually;
3. keep both building the whole time;
4. compare them running, and only then keep one.

The rename was what stopped step 3 from being real: one module cannot be two
drivers, and `-Dusb2otg=dwc2emu68` made the inherited engine build under the
rewrite's name. They are now separate:

| | `soc/usb/usb2otg` | `soc/usb/dwc2emu68` |
|---|---|---|
| what | the AROS DWC2 engine, adopted | the Bellatrix rewrite |
| source | 394 KB, 7 files | 62 KB, 8 files |
| module | `usb2otg.device`, 62 KB | `dwc2emu68.device`, 26 KB |
| metatarget | `kernel-usb-arosotg` | `kernel-usb-dwc2emu68` |

`kernel-usb-arosotg` and not `kernel-usb-usb2otg`: arm-native already defines
that name for the original of this code, and mmake matches metatargets across
the whole tree -- asking for it here built arm-native's copy and died on a
`yield` instruction the m68k assembler does not have. `build-aros.sh` builds
both, because building only one is how the other stops being code.

## How far the port actually is

Measured from the command dispatch, not from intent:

| command | `usb2otg` | `dwc2emu68` |
|---|---|---|
| `UHCMD_CONTROLXFER` | `cmdControlXFer` | implemented, root hub and devices |
| `UHCMD_INTXFER` | `cmdIntXFer` | implemented, root hub and devices |
| `UHCMD_BULKXFER` | `cmdBulkXFer` | falls to `default:` -> `IOERR_NOCMD` |
| `UHCMD_ISOXFER` | `cmdIsoXFer` | falls to `default:` -> `IOERR_NOCMD` |

So the rewrite covers HID -- control plus interrupt, which is keyboard and
mouse -- and cannot do storage, audio or video. The adopted engine is the only
side with full coverage, which is the concrete reason not to delete it yet.

**A defect found while establishing this:** the rewrite's `NSCMD_DEVICEQUERY`
table listed `UHCMD_BULKXFER` and `UHCMD_ISOXFER` as supported while the
switch answered `IOERR_NOCMD` to both -- the device promised Poseidon
transfers it then refused. The table now lists only what the switch does, with
a note to add each back in the same change that implements it.

**USB remains disabled**, separately and deliberately: the
`kernel-usb-m68k-emu68` alias is commented out and `build-aros.sh` deletes
`Devs/USBHardware` after building, so both drivers keep compiling without
going on a card. Re-enabling is a decision of its own.

# Remaining work

- port `cmdBulkXFer`, then `cmdIsoXFer`, adding each to the DEVICEQUERY table
  in the same change;
- re-enable USB and enumerate a keyboard under QEMU with each driver;
- compare, and delete one.

# Final implementation

The `dwc2emu68.device` module is what builds, and end-to-end input remains
under investigation -- it has never been shown to enumerate a keyboard or
mouse all the way through. It:

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
