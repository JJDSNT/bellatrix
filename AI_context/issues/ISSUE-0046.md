---
id: ISSUE-0046
title: "Integrate the Raspberry Pi Bluetooth UART with the native AROS Bluetooth stack"
status: doing
priority: high
type: feature
owner: agent
created_at: 2026-08-21
updated_at: 2026-08-24
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

- 2026-08-22 -- **The submodule is gone and the transport exists.**
  `bthciuart.device` (`soc/bluetooth/bthciuart/`) presents `btuart.resource`
  to AROS's stack as an HCI device in `DEVS:Bluetooth`: H4 framing and the
  AROS device protocol, nothing about the board. `external/aros-bluzing` was
  removed after checking that nothing in it was still ours -- the stack is
  upstream and ahead (same code, headers renamespaced `bluetooth/` ->
  `btcore/`, plus LE scan and LE security encoders we did not have), and
  BTScan cannot come across because it is written against the bluzing port's
  own manager task and UART transport, which is the architecture being
  replaced. AROS ships `Prefs/Bluetooth` for that half.

  The card now carries one Bluetooth: `bluetooth.library`, `BTStackLoader`,
  `bthid.class`, the firmware loaders, `vbthci.device` and `bthciuart.device`.
  A stale `Prefs/Env-Archive/SYS/Packages/aros-bluzing` was removed with it --
  a card shipping that has the Startup-Sequence looking for a package that is
  not there.

  **None of it has run.** `C:BTStackLoader` is called by the stock
  Startup-Sequence, so the next boot is the first time this path has all its
  pieces. Three things are unknown and none should be assumed: whether the
  stack picks our device over `vbthci`, whether `btuart.resource` yields the
  PL011 when the unit task claims it, and whether a BCM43438 answers HCI at
  all before a patchram upload -- which this port still does not do.

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

# The radio answers, 2026-08-24

On a Raspberry Pi 3, with `AddBTHardware` added to `S:Startup-Sequence`
(`patches/aros/0056`) and the transport write contract fixed:

    Adding hardware DEVS:Bluetooth/bthciuart.device, unit 0...okay!
    ...bring-up complete - HCI 7.0 LMP 7.8713, BR/EDR+LE, addr AA:AA:AA:AA:AA:AA
    ...ready: AA:AA:AA:AA:AA:AA, HCI 4.1, LMP 4.1 (Broadcom), BR/EDR + LE
    New hardware ... added (Raspberry Pi onboard Bluetooth, ...)

The controller resets, reports its version, features and buffer sizes, and the
stack registers it. Steps 0 through 13 of the HCI bring-up all return ok.

**What blocks a mouse is step 15.** Counting `enum` HCB_* in
`bluetooth/hwtask.h`, step 14 is `HCB_READ_LOCAL_NAME` and step 15 is
`HCB_WRITE_SCAN_ENABLE`, and both time out. Write_Scan_Enable is what turns on
page scan and inquiry scan; without it the controller does not answer paging,
which is exactly the failure seen when connecting a mouse. Step 13
(`WRITE_LOCAL_NAME`) succeeded and step 14 only reads that name back, so the
pair reads as "the controller stopped answering here" rather than as two
commands being special.

**And the address is not real.** `AA:AA:AA:AA:AA:AA` is what a BCM43438 that
has never been given a patchram reports; the true BD_ADDR arrives with the
firmware. An invalid address breaks pairing independently of the scan problem.

## What the legacy branch says about firmware

**It never uploaded any.** `src/io/bluetooth/bt_firmware_stub.c` on `legacy`
is, in full:

    const uint8_t brcm_patchram_buf[] = {};
    const int brcm_patch_ram_length = 0;
    const char brcm_patch_version[] = "none";

and `patches/0005-btstack-baremetal-bcm-init.patch` logs "bcm: no init script
present, continuing without PatchRAM upload". That build talked to the
controller and scanned. So patchram is **not** the blocker, and attributing
the AA:AA:AA:AA:AA:AA address to its absence was wrong -- that address needs
its own explanation.

What legacy did have, in `bt_hal_raspi3.c`, is an RX ring fed by a bounded
interrupt top half, with `s_rx_ring_overflow` counting bytes lost when the
ring filled. Our `btuart.resource` does arm an RX interrupt
(`PL011_IFLS_RX18`, a quarter of the 32-byte FIFO) so that is not the
difference -- but it has **no overflow counter**, so if bytes are being
dropped nothing says so.

That is worth chasing because of *where* the bring-up stops. Steps 0-13 are
short commands with short replies. Step 14 is `READ_LOCAL_NAME`, whose
response carries **248 bytes** of name field -- the first reply in the
sequence that cannot fit in the PL011's FIFO. A receive path that loses bytes
under a long reply would fail exactly here, and would then desynchronise the
command stream, which is what step 15 timing out as well looks like.

Not established. But it is a testable hypothesis and the counter to test it
with is one the legacy branch already had.

So the open threads are:

1. why the command stream stops responding after step 13 -- flow control, a
   missing event, or a response the parser drops. This is ours to find and
   does not need firmware.
2. the patchram. We have no `.hcd` and no vendor upload protocol, which is
   what a real BD_ADDR needs.

**A parsing bug is visible in the same two lines**: one says HCI 7.0 /
LMP 7.8713, the next says HCI 4.1 / LMP 4.1 for the same controller. At least
one of them is misreading the Read_Local_Version response.

# Corrected 2026-08-24

Two of the three items below are no longer true, and the issue was read as
current for three days after they stopped being so.

**The transport exists.** `aros/arch/m68k-emu68/soc/bluetooth/bthciuart` builds
`Devs/Bluetooth/bthciuart.device` -- H4 framing over the PL011 that
`btuart.resource` owns -- and `build-aros.sh` builds it. It is on the card.

**There is only one bluetooth.library on the card.** The
`kernel-bluetooth-m68k-emu68` alias is enabled, so `Libs/bluetooth.library`
(176 KB) is AROS's own; nothing on the card comes from `Extras/aros-bluzing`.
Which means item 3 -- removing the `external/aros-bluzing` submodule -- is
unblocked and only wants doing.

**What actually remains is firmware.** A BCM43438 runs from ROM until it is
sent a patchram `.hcd`, we have no such blob (the WiFiPi submodule's firmware
directory has none either), and the Broadcom vendor protocol that uploads one
lives outside both the transport and the resource.

**And there is a way to make progress without a Pi**, which nothing here has
tried: `vbthci.device` is a *virtual* HCI transport, and the card already
carries `AddBTHardware`, `BTStackLoader`, `BTDevLister` and `BTErrorLog`.
Loading the stack against vbthci under QEMU separates "the stack works" from
"the radio needs firmware" -- worth knowing before investing in patchram.

# What was left, as written on 2026-08-21

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
