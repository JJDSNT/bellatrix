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
- a second board is a stated direction for this port, so a driver that
  already knows three SoCs is worth more than one narrowed to ours;
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

# Neither driver plays, 2026-08-24 (corrects the section below)

PWM does **not** work. An earlier report that it did -- which the whole
section below reasons from -- turned out to be a system beep, not a played
file; a WAV through `C:Play` produces nothing on either driver.

That moves the fault out of the HDMI-specific path. `hdmiaudio` and
`pwmaudio` share exactly one significant dependency, and both open it:
**dma.resource**. If nothing is feeding the FIFO, neither driver can produce
sound regardless of how correctly its control registers are programmed.

This is also what the legacy driver warns about in `src/host/raspi3/
hdmi_audio.c`: "CPU-polled feeding of MAI_DATA produced a discontinuous IEC958
stream that the sink muted; the DMA's HDMI DREQ paces the FIFO steadily."
The DMA is not an optimisation here, it is what makes audio exist.

**Next step is therefore the DMA path, not the MAI registers**: does the
driver obtain a channel, does the control block get built, does the transfer
start, does DREQ pace it. None of that is instrumented. The MAI_CTL finding
below stays on the list -- it is still a real discrepancy against a validated
driver -- but it cannot be the reason PWM is silent, and fixing it first
would have proved nothing.

Method note, since it cost a whole investigation: "PWM works" was taken as
given and every inference was built on it. A single WAV through C:Play would
have tested it at any point.

# The MAI_CTL bit map is wrong for this SoC, 2026-08-24

HDMI audio is silent on a Pi 3 while PWM works. Instrumentation showed
everything reachable to be correct -- periiobase 0xf2000000, N=6272,
SMP=000e8000 (163680000/44100 = 3712), CTS 165000 after a rounding fix,
SCHED=000cb02b and RAMPKT_STA=00000015 with the audio infoframe transmitting
in slot 4. Only the control register looked odd, and it is.

The `legacy` branch has a validated HDMI audio driver for this exact silicon
(`src/host/raspi3/hdmi_audio.c`; it played a stereo chirp). Its MAI_CTL bit
map does not match the one we inherited from AHI's RPiHDMI:

| bit | legacy (BCM2837, validated) | RPiHDMI (ours) |
|---|---|---|
| 0 | `ENABLE` | `RESET` |
| 1 | `BUSY` | `ERRORF` |
| 2 | `CHALIGN` | `ERRORE` |
| 3 | -- | `ENABLE` |
| 8 | `FLUSH` | `PAREN` |
| 18 | `RESET` | -- |
| 19 | `CHNUM` shift | -- (CHNUM at 4) |
| 24 | `WHOLSMP` | -- |

Hardware reported `MAI_CTL=00003428`. Read with the legacy map, **bit 0 is
clear: the MAI was never enabled.** We write ENABLE into bit 3, which is not
what enables it on this part.

RPiHDMI is written for BCM2711/2712, where the register was laid out
differently. The SoC table we kept carries the right *offsets* for BCM283x and
the wrong *bit definitions*, which is why every other value checked out.

Two more findings from the same comparison, both from the legacy driver's own
comments:

- **`EXTERNAL_CTS_EN` does not mean "ignore the hardware"**. The block
  measures CTS against the live pixel clock and treats the written value as a
  seed, "which makes a fixed CTS seed valid regardless of the actual
  pixel/HSM clock". That is the mechanism that survives a resolution change.
  It was briefly cleared here on the opposite theory and has been restored.
- **CPU-polled feeding does not work**: "CPU-polled feeding of MAI_DATA
  produced a discontinuous IEC958 stream that the sink muted; the DMA's HDMI
  DREQ paces the FIFO steadily." Worth confirming our DMA path does the same
  once the control register is right.

# Verification

Only on hardware:

1. `AudioModes` shows HDMI Audio / I2S Audio / PWM Audio in prefs;
2. a Play16-style test produces sound over HDMI;
3. `config.txt` needs `hdmi_drive=2` -- the 2026-07 legacy work established
   that, and it is already in `arch/arm-raspi`'s generated config.

The legacy tree got as far as a stereo chirp playing over HDMI, so the output
path is known to work in principle. That was a different build.
