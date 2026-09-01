# BCM283x Bluetooth

Three modules, and the names say which layer each one is:

| module | layer | what the name states |
|---|---|---|
| `pl011bt.resource` | board | the PL011 block it owns, as distinct from the AUX mini-UART Emu68 talks on |
| `h4bthci.device` | transport | the protocol it speaks -- H:4 framing -- not the wire it happens to sit on. Parallels upstream's `vbthci.device`, where `v` is virtual |
| `brcmbt.fwl` | firmware | vendor plus Bluetooth, the same shape as upstream's `rtlbtv1`/`rtlbtv2`. Not `brcmfw`: on a Pi the SoC and the radio are both Broadcom, so "brcm firmware" does not say which |

They were `btuart.resource`, `bthciuart.device` and `brcmfw` until 2026-09-01.
Those names had the specificity in the wrong layer: the board-specific
resource carried the generic name and the genuinely generic transport carried
the name Linux uses for its generic one. The rule the rename follows is the
one Emu68 applies in `src/boards/` -- `brcm-emmc.device`, `brcm-sdhc.device`,
`gic400.library`, `unicam.resource` -- name the block, and keep a generic name
only for something that is generic. A module name escapes into a flat
namespace (`libbase`, the mmake target, the file in `DEVS:`), so the
directory it sits in cannot carry that for it.

## The resource

This directory owns the Bellatrix/AROS side of the Raspberry Pi onboard
Bluetooth transport. It deliberately stops at the board boundary: PL011,
GPIO, clock, interrupt and device-tree handling belong here, while HCI framing
belongs to `h4bthci/` beside it, and the Bluetooth protocols to AROS's own
stack in `rom/bluetooth`. Broadcom patchram belongs to neither yet.

The resource exposes exclusive claim/release, PL011 configuration,
`BT_REG_EN` control and non-blocking polling reads/writes. Configuration also
routes GPCLK2 to GPIO 43 as the controller's 32.768 kHz LPO. Resident
initialisation only discovers the peripheral window and UART clock; it does
not touch PL011 until a client claims and configures it. Emu68 diagnostics
permanently use AUX mini-UART.

Planned bring-up order:

1. route the PL011 interrupt to the m68k guest;
2. add asynchronous RX/TX queues and signal the owning task;
3. drop `h4bthci.device`'s polling loop onto those queues.

Step 3 used to say "connect `aros-bluzing/ports/aros/transport-uart` to this
resource". That submodule is gone: AROS HEAD imported the same stack into
`rom/bluetooth`, and the transport that consumes this resource is
`h4bthci/`, which presents it to that stack as an HCI device in
`DEVS:Bluetooth`.

Firmware files and the Broadcom vendor protocol are intentionally not part of
this resource.
