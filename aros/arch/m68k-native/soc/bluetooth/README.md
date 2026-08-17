# BCM283x Bluetooth UART resource

This directory owns the Bellatrix/AROS side of the Raspberry Pi onboard
Bluetooth transport. It deliberately stops at the board boundary: PL011,
GPIO, clock, interrupt and device-tree handling belong here, while HCI framing,
Broadcom patchram and the Bluetooth protocols belong in `aros-bluzing`.

The resource exposes exclusive claim/release, PL011 configuration,
`BT_REG_EN` control and non-blocking polling reads/writes. Configuration also
routes GPCLK2 to GPIO 43 as the controller's 32.768 kHz LPO. Resident
initialisation only discovers the peripheral window and UART clock; it does
not touch PL011 until a client claims and configures it. Emu68 diagnostics
permanently use AUX mini-UART.

Planned bring-up order:

1. route the PL011 interrupt to the m68k guest;
2. add asynchronous RX/TX queues and signal the owning task;
3. connect `aros-bluzing/ports/aros/transport-uart` to this resource.

Firmware files and the Broadcom vendor protocol are intentionally not part of
this resource.
