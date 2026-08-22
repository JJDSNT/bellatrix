# BCM283x Bluetooth UART resource

This directory owns the Bellatrix/AROS side of the Raspberry Pi onboard
Bluetooth transport. It deliberately stops at the board boundary: PL011,
GPIO, clock, interrupt and device-tree handling belong here, while HCI framing
belongs to `bthciuart/` beside it, and the Bluetooth protocols to AROS's own
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
3. drop `bthciuart.device`'s polling loop onto those queues.

Step 3 used to say "connect `aros-bluzing/ports/aros/transport-uart` to this
resource". That submodule is gone: AROS HEAD imported the same stack into
`rom/bluetooth`, and the transport that consumes this resource is
`bthciuart/`, which presents it to that stack as an HCI device in
`DEVS:Bluetooth`.

Firmware files and the Broadcom vendor protocol are intentionally not part of
this resource.
