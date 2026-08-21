# VC4 validation

Bellatrix links `vc4gfx.hidd` as the native m68k-emu68 display driver. It does
not use Picasso96. QEMU's Raspberry Pi model implements the firmware mailbox
framebuffer but not the HVS, so the two supported runtime paths have different
expected evidence:

- QEMU: framebuffer allocation and page offsets through the property mailbox;
  `HVS_ID` reads zero and direct HVS takeover is skipped.
- Raspberry Pi 3: the same framebuffer allocation, followed by ownership of
  the live HVS display list and a calibrated PixelValve vsync interrupt.

## QEMU gate

Capture serial output and run the checker:

```bash
./run.sh --headless --serial /tmp/bellatrix-vc4-qemu.log
./scripts/check-vc4-log.sh qemu /tmp/bellatrix-vc4-qemu.log
```

The graphical gate is a screendump after Wanderer starts. It must have correct
colours and no duplicated splash, horizontal divider, or BootUI clock over the
desktop. The serial checker proves that BootUI retargeted to BGRX8888, stopped
at the direct-scanout hand-off, and that QEMU selected the intended mailbox
fallback.

## Raspberry Pi 3 gate

Generate the real-hardware pack:

```bash
./scripts/make-sdcard.sh --pi --pack
```

Extract `out/aros/bellatrix-pi3.tar.xz` at the root of the Pi's existing FAT32
boot partition, following the normal Bellatrix/Emu68 card workflow:

```bash
tar -xJf out/aros/bellatrix-pi3.tar.xz -C /media/you/BOOT
```

Replace `/media/you/BOOT` with the actual mounted partition. Connect a 3.3 V
USB-to-UART adapter to GPIO 14/15 and ground, boot with the HDMI monitor already
on, save the AUX mini-UART output as a file, then run:

```bash
./scripts/check-vc4-log.sh pi pi3-vc4.log
```

The hardware gate requires all of the following in the same boot:

- BootUI retarget and a clean direct-scanout hand-off;
- `takeover: ACTIVE` with nonzero display-list and matching framebuffer data;
- a selected vsync bit reporting ticks per frame;
- live, nonzero vsync ticks after the takeover;
- Wanderer reached without FBALLOC, flip, or VC4 DMA failure.

Also inspect HDMI output. A serial pass cannot prove colours, cursor movement,
absence of tearing, or that later desktop drawing remains stable. Preserve the
complete log and a photograph or capture of the final desktop as the hardware
evidence for `AI_context/issues/ISSUE-0043.md`.
