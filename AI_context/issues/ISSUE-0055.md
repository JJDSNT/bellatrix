---
id: ISSUE-0055
title: "Audio exists on this port for the first time, and is unproven"
status: open
priority: low
type: feature
owner: unassigned
created_at: 2026-08-24
updated_at: 2026-08-24
tags:
  - audio
  - ahi
  - hdmi
  - m68k-emu68
blockers:
  - needs real hardware to test
related_files:
  - aros/workbench/devs/AHI/Drivers/hdmiaudio
  - aros/workbench/devs/AHI/Drivers/i2saudio
  - aros/workbench/devs/AHI/Drivers/pwmaudio
  - patches/aros/0054-ahi-build-the-bellatrix-audio-drivers-on-m68k.patch
---

# Summary

Until 2026-08-24 this port had no audio driver of any kind. `Devs/AHI` held
ten drivers, none of which could reach this hardware: AHI gates its Raspberry
Pi drivers on `proto/dma.h`, which this target has, and
`$(filter arm aarch64,$(host_cpu))`, which it does not pass.

`hdmiaudio`, `i2saudio` and `pwmaudio` are our copies of `RPiHDMI`, `RPiI2S`
and `RPiPWM`, adopted the way the USB, VideoCore and SD card drivers were and
linked into the submodule by `setup.sh`.

**None has ever produced a sound.** QEMU's raspi3b emulates neither the MAI
block, nor I2S, nor PWM audio, so all three can only be exercised on a Pi.

# What porting actually required

Very little, because the drivers were written portably -- they already used
`AROS_LE2LONG`/`AROS_LONG2LE` for the little-endian registers.

- `hdmiaudio` and `pwmaudio` emitted `dsb sy` / `dmb sy` around every register
  access. Those became the compiler barriers
  `arch/m68k-emu68/include/asm/cpu.h` already defines for the DMA and USB
  paths: under Emu68 no guest instruction orders the host's memory system --
  the JIT and the host CPU do -- and what is still needed is to stop the
  compiler moving an access across the barrier.
- `i2saudio` had no assembly at all and needed nothing.
- Each got its own `AHIDB_AudioID` (0x00420001 / 0x00430001 / 0x00440001) so
  ours cannot collide with each other or with the drivers they came from.

# The multi-SoC template

`hdmiaudio` carries all three SoC tables, including the Pi 4 and Pi 5 ones
this board will not use. That was briefly removed and then restored, and the
reasons for restoring are worth keeping:

- `proto/openfirmware.h` is present for this target, so the device-tree probe
  those SoCs need compiles here;
- [ISSUE-0069](ISSUE-0069.md) already has a second board in view;
- deleting them widens the gap with the upstream driver, turning its future
  fixes into something to re-derive rather than apply.

It also leaves a worked example of the shape a multi-SoC driver takes here:

| file | SoC | selected by |
|---|---|---|
| `rpihdmi-bcm283x.c` | Pi 2 / Pi 3 | `KATTR_PeripheralBase` |
| `rpihdmi-bcm2711.c` | Pi 4 | device tree, `brcm,bcm2711-hdmi0/1` |
| `rpihdmi-bcm2712.c` | Pi 5 | device tree, `brcm,bcm2712-hdmi0/1` |

Each is one `struct RPiHDMISoc` of register offsets, DMA DREQ, HSM clock and
`init`/`stop` hooks; all transfer code is common. Adding a SoC is adding a
file with a table.

# What this port cost elsewhere, and gained

It found `BCM2836_PERIPHYSBASE` missing from
`arch/m68k-emu68/include/hardware/bcm2708.h` -- 0x3f000000, the peripheral
base of the Pi 3, this machine's own. That header has since been adopted whole
from `arm-native` (101 defines to 379) rather than continuing to grow one
constant per failed port. See the commit for
`feat(wifi): port the AROS Broadcom driver, and adopt the whole BCM283x header`.

# Verification

Only on hardware:

1. `AudioModes` shows HDMI Audio / I2S Audio / PWM Audio in prefs;
2. a Play16-style test produces sound over HDMI;
3. `config.txt` needs `hdmi_drive=2` -- the 2026-07 legacy work established
   that, and it is already in `arch/arm-raspi`'s generated config.

The legacy tree got as far as a stereo chirp playing over HDMI, so the output
path is known to work in principle. That was a different build.
