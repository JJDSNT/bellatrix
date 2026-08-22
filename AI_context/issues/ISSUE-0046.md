---
id: ISSUE-0046
title: "Integrate the Raspberry Pi Bluetooth UART with the native AROS Bluetooth stack"
status: doing
priority: high
type: feature
owner: agent
created_at: 2026-08-21
updated_at: 2026-08-21
tags:
  - bluetooth
  - btuart
  - raspberry-pi-3
  - aros-head
blockers:
  - "confirm that the Bellatrix update to the current AROS HEAD builds and boots correctly"
related_files:
  - aros/arch/m68k-emu68/soc/bluetooth
  - external/aros/rom/bluetooth
  - external/aros-bluzing/ports/aros/transport-uart
  - external/aros-bluzing/ports/aros/uart-selftest
  - external/aros-bluzing/protocols/vendor_init/broadcom
---

# Summary

After the Bellatrix update to the current AROS HEAD is validated, connect the
existing Raspberry Pi 3 `btuart.resource` to the Bluetooth stack now maintained
in the main AROS tree. The standalone `aros-bluzing` integration is deprecated;
its UART and Broadcom work remains reference material, not a component to inject
under `contrib`.

# Problem

AROS now owns the Bluetooth core and its native integration under
`rom/bluetooth`. It provides `bluetooth.library`, Bluetooth classes, preferences,
USB registration, virtual HCI hardware and loadable Realtek firmware handlers.
It does not yet provide a real H4 UART hardware backend for the Raspberry Pi 3
onboard controller.

Bellatrix already has the platform boundary needed for that backend:
`btuart.resource` owns the PL011, routing, clock, power and interrupt-driven RX
ring. The old standalone Bluzing tree also contains a working direction for the
transport and hardware tests, but its Manager Task, library, package startup and
USB integration have been superseded by the native AROS design.

# Goal

Implement the following native path without restoring the old contrib package:

```text
Bellatrix btuart.resource
        -> native AROS Bluetooth UART hardware module
        -> bluetooth.library hardware registration/API
        -> H4 and btcore code from rom/bluetooth/stack
        -> bthid.class and Prefs/Bluetooth
```

# Reusable work

- `ports/aros/transport-uart`: reference for H4 RX/TX pumping, timing and error
  handling over `btuart.resource`.
- `ports/aros/uart-selftest`: reference for staged PL011/H4/controller bring-up
  validation before enabling the complete stack.
- `protocols/vendor_init/broadcom`: requirements and protocol notes for a future
  Broadcom `.hcd` firmware loader.
- `ai-context/Some_stuff.md`: architectural requirements for identity,
  dual-mode devices, remote initiation, persistence and reconnection.
- The portable core and host tests: already imported into
  `rom/bluetooth/stack`; use and extend that copy rather than importing it again.

# Work that must not be reintroduced

- The standalone Bluzing `bluetooth.library`, Manager Task and package startup.
- The contrib-only build and installation model.
- The old USB transport, because the main AROS USB Bluetooth class now registers
  controllers with `bluetooth.library`.
- `btscan` as the primary UI, because the native `Prefs/Bluetooth` replaces it.
- A second copy of the portable Bluetooth core.

# What is left

1. Finish and validate the Bellatrix update to the current AROS HEAD.
2. Study the hardware contract used by `rom/bluetooth/hardware/vbthci` and the
   current USB Bluetooth registration path.
3. Design a native UART hardware module around `btuart.resource`, retaining the
   main tree's ownership, object and event model.
4. Port only the useful H4 transport mechanics from the standalone tree.
5. Adapt the UART self-test to the native stack and validate controller reset,
   event reception, discovery and clean shutdown on a Pi 3.
6. Add a native Broadcom firmware loader following the new pluggable loader API,
   if the onboard controller requires `.hcd` upload for full operation.
7. Revisit remaining architectural gaps, especially LE SMP wiring and IRK-based
   resolvable-private-address identity resolution.

# Decisions taken

- The AROS main-tree Bluetooth implementation is authoritative.
- Bellatrix-specific platform access remains behind `btuart.resource`.
- New AROS-side integration will be maintained as a numbered Bellatrix patch or
  proposed upstream; `external/aros` will not be edited as an unrecorded fork.
- The old standalone repository is historical/reference material only.

# Acceptance criteria

- [ ] The AROS HEAD refresh builds, packages and boots before this work starts.
- [ ] The onboard Pi 3 controller registers through the native
      `bluetooth.library` hardware model.
- [ ] H4 command, event and ACL traffic operates without RX-ring loss or stale
      ownership of `btuart.resource`.
- [ ] Native discovery sees real devices on Pi 3 hardware.
- [ ] A Bluetooth HID device works through `bthid.class`.
- [ ] Shutdown and restart release all UART, task and message resources cleanly.
- [ ] Any required Broadcom firmware is loaded through the native pluggable
      firmware interface.

# Execution log

- 2026-08-22 -- **AROS HEAD ships the stack, and it builds for m68k unchanged.**
  The 2026-08-21 pin refresh brought `rom/bluetooth` -- library, stack,
  classes, firmware, prefs, and one HCI device -- and `rom/bluetooth/stack`
  carries `LICENSE.aros-bluzing`, so it is the same code we vendored, imported
  upstream.

  It was never built here because `rom/` and `workbench/` select it through
  `kernel-bluetooth-$(AROS_TARGET_ARCH)-$(AROS_TARGET_CPU)` and this target
  defined no such alias -- the same per-target pattern as USB. That absence is
  what made the submodule look necessary: the card carries a
  `bluetooth.library` from `Extras/aros-bluzing` because nothing was putting
  AROS's own there.

  One line in `arch/m68k-emu68/mmakefile.src` turns it on, and it compiles
  with no adaptation at all:

      Libs/bluetooth.library        172 KB
      Devs/Bluetooth/vbthci.device   28 KB
      Devs/Bluetooth/FWLoaders/
      Prefs/Bluetooth
      C/BTStackLoader

  `C:BTStackLoader` is already invoked by the stock Startup-Sequence, so
  nothing of ours has to start it.

# What is left, after this

1. **The transport.** The stack talks to HCI devices in `DEVS:Bluetooth`, and
   the only one upstream is `vbthci`, which is virtual.
   `rom/bluetooth/hardware/vbthci/mmakefile.src` is the model:
   `%build_module modtype=device moduledir=Devs/Bluetooth uselibs="btcore"`.
   The Pi's onboard controller sits on the PL011 that
   `soc/bluetooth/btuart.resource` already owns -- exclusive claim, PL011
   configuration, `BT_REG_EN`, GPCLK2 as the 32.768 kHz LPO, polled read and
   write. What does not exist is the device that presents that as HCI.
2. **Two bluetooth.library, one system.** Enabling the alias and keeping the
   `Extras/aros-bluzing` package puts two of them on the card. The package
   goes when the transport works, not before -- it is what BTScan currently
   binds to.
3. **The submodule.** `external/aros-bluzing` can be removed once nothing on
   the card comes from it. `scripts/setup.sh` and
   `soc/bluetooth/README.md` name it and will need updating; the README's
   planned bring-up order still says "connect aros-bluzing/ports/aros/
   transport-uart to this resource", which is now the wrong target.

- 2026-08-21 — Compared the deprecated standalone Bluzing tree with AROS HEAD.
  Confirmed that the portable core and tests were incorporated into
  `rom/bluetooth/stack`, while UART hardware support remains the principal
  Bellatrix-specific reusable area. Recorded this as the next movement after
  validation of the AROS HEAD refresh.
