---
id: ISSUE-0055
title: "Audio on this port: HDMI confirmed working"
status: resolved
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

# Three corrections against the legacy driver, 2026-08-24

Comparing register by register against `src/host/raspi3/hdmi_audio.c` on the
`legacy` branch -- the only HDMI audio driver that has produced sound on this
silicon -- turned up three differences. Two of the checks came back clean and
are recorded so they are not re-run:

- **`MAI_CTL` is identical.** An earlier note here claimed the bit map was
  wrong for BCM283x. The `#define`s do differ between the trees, but both
  drivers *write* `(1<<3)|(2<<4)|(1<<12)|(1<<13)` = `0x3028`. The `0x3428`
  seen in the readback has bit 10 set by the hardware. Retracted.
- **`MAI_CONFIG` is identical** -- `0x0C000003` on both sides -- and **the CTS
  arithmetic is right**: `f_pixel * N / (128 * fs)` gives 165000 at 148.5 MHz
  and 44.1 kHz, which is what the driver logged.

The three that were wrong:

## 1. The IEC958 framing was being written twice

`rpihdmi-iec958.c` built complete subframes: preamble in bits 3:0, validity,
user and channel-status at 30:28, even parity at 31, around the sample at
27:12. With `MAI_CONFIG` programmed as it is, **the MAI block writes those
same bits itself**. The legacy driver says so plainly --

    (void)subframe_in_block; /* HW does IEC958 framing; no software block counting */

-- and writes only the sample, low nibble cleared. Software framing on top of
hardware framing corrupts the stream a sink is entitled to mute.

The file now emits the proven word format and nothing else; the channel-status
blocks and the 192-frame counter went with it, since nothing reads them. The
sample position was already correct on both sides -- the bug was the extra
bits, not where the sample sat.

## 2. `MAI_THR` was set from a table this board does not match

The SoC table asks 0x10 in all four DREQ/PANIC fields; the legacy driver wrote
`0x08080608`. These are the levels at which the block raises DREQ, so a
threshold the FIFO never reaches is a DREQ that never fires -- and a DMA
channel that sits ACTIVE forever waiting for one, which is exactly what both
audio paths were observed doing. Now `MAI_THR_PROVEN`.

## 3. `hsm_clock` in the SoC table is wrong, so every N was wrong

`MAI_SMP` holds N at 31:8 and M at 7:0, and N/(M+1) is the HSM clock over the
sample rate. We computed it as `hsm_clock / samplerate` with M = 0, from the
table's 163680000. The legacy driver writes `0x0DCD21F3` at 48 kHz: N=904481,
M=243, a ratio of 3722.14, implying an HSM clock near **178.66 MHz** -- 9% off
what the table claims. The legacy driver also notes it could never read the
HSM rate back, which is presumably how the table's figure came to be a guess.

Rather than recompute from a number that cannot be trusted, the driver now
scales the proven one: M is kept and N derived from the 48 kHz value,

    N = (0x0DCD21 * 480) / (samplerate / 100)

which reproduces `0x0DCD21F3` exactly at 48 kHz and stays on the same clock at
other rates. Multiplying by 480 and dividing by samplerate/100 keeps every
intermediate inside 32 bits and avoids the division error a /1000 introduces
at 44.1 kHz.

## Status

All three are in the pack of 2026-08-24 23:20 and **none of them is verified**:
this can only be tested on hardware. Item 2 is the one that would also explain
the PWM path's stuck DMA, but the PWM driver has its own thresholds and has not
been touched here.

# Second round, same day: the mailbox call was corrupting memory

The log from the pack above stops dead one line after the new `MAI_SMP`, and
never reaches the CTS report. The only thing between them is
`hdmi_pixel_clock_hz()`, which this session had added, and it was wrong in two
independent ways:

- **the message lived on the stack.** The mailbox register carries the buffer
  address in bits 31:4 and the channel in 3:0, so an address that is not
  16-byte aligned has its low nibble taken as the channel and the firmware
  writes its 32-byte reply *somewhere else*. That is a silent write into
  whatever sat near the stack. The caller only ever saw a reply address that
  did not match and returned 0 -- which is exactly the "the mailbox returns 0"
  symptom recorded earlier, misread at the time as the firmware declining to
  answer.
- **it split the transaction** into `MBoxWrite` + `MBoxRead`. A concurrent
  mailbox user takes the reply in between.

`pwmaudio` already had this right, and its comment says both things in as many
words. `hdmiaudio` now uses the same mechanism: `MBoxCall`, a heap buffer
aligned to and confined within one 64-byte cache line, and a reply accepted
only when the address, the response code and the tag-processed bit all agree.

`dma_dreq = 17` was checked against the legacy driver at the same time and is
correct -- `DMA_TI_PERMAP_HDMI (17u << 16) /* DREQ peripheral 17 = HDMI */`.

# The PWM "stuck DMA" was an artefact of where the log sampled

Recorded because it sent this investigation the wrong way once already.

    [pwmaudio] DMA start: CS=10f80021 ... ACTIVE
    [pwmaudio] before FIFO enable: CTL=00008181
    [pwmaudio] after  FIFO enable: CTL=0000a1a1

`CS` bit 3 (DREQ) reads clear, and that looked like a channel waiting forever
on a request line. But `CS` is sampled at arm time, *before* `USEF1`/`USEF2`
are set -- `CTL=0x8181` has no USEF, `0xa1a1` does. Until then the PWM is in
DAT mode and has no reason to raise a FIFO DREQ, so a clear bit there means
nothing. Nothing in the log sampled the channel after the switch.

`pwm_report_state_after()` now takes the channel and reads `CS` and
`TXFR_LEN` twice, a millisecond apart, printing `DRAINING` or `STALLED`.
`TXFR_LEN` counts down as words leave, so this distinguishes the two outright
instead of inviting the inference again.

The claim "the DMA waits on a DREQ the PWM never raises" is therefore
**withdrawn** -- it was never measured. The next boot log decides it.

# The actual reason no Pi audio driver has ever made a sound

`TXFR_LEN=0->0 STALLED` is what the new PWM instrumentation reported, and the
value that matters is not the `STALLED` label but the **0**. It is not a
counter failing to decrease: the channel is ACTIVE and its transfer length is
zero, which means the engine fetched a control block full of zeroes.

Two defects, both inherited unexamined from the arm-native drivers these were
copied from, and both invisible on the CPU they were written for.

## 1. The DMA was given m68k virtual addresses

    #define GPU_BUS_ADDR(x) BCM2708_DMA_BUS_ADDR(x)   /* 0xC0000000 | x */

On arm-native the kernel identity-maps low memory, so the virtual address and
the ARM physical address are the same number and `| 0xC0000000` is the whole
translation. Under Emu68 they are not the same number. Every control block
address, every sample buffer address and every chained `nextconbk` pointed at
whatever that number happened to land on. `CB=c5512f20` in the log is
`0xC0000000 | 0x05512f20`, and `0x05512f20` is where the *m68k* holds the
block.

The sdcard driver in this tree has always done it correctly:

    BCM2708_DMA_BUS_ADDR((ULONG)(IPTR)KrnVirtualToPhysical(virt))

All three audio drivers now do the same. Each already opened
`kernel.resource`; only the translation was missing.

## 2. No control block field was byte-swapped

The DMA engine reads control blocks little-endian. `cb->txfr_len = len` on
m68k stores big-endian. The sdcard driver writes every field through
`AROS_LONG2LE()`; the audio drivers wrote all six raw. 31 field writes across
the three drivers are now wrapped.

Either defect alone is fatal, which is why no amount of work on the MAI
registers was ever going to produce sound: the register programming was being
debugged while the data path could not move a byte.

## What this explains

- PWM "only beeps, does not play the WAV". The beep is `pwm_ramp_dc()` and the
  DAT-mode writes -- CPU stores straight to `PWM_DAT`, which work. The WAV goes
  through DMA, which never moved.
- HDMI silent with every MAI register correct.
- I2S never tested, and it had the same two defects.

## Still unverified

Only hardware decides. The next log should show `TXFR_LEN` counting down and
`DRAINING` instead of `STALLED`.

# Resolved on hardware, 2026-08-24: HDMI audio plays

The user confirms sound over HDMI on a Raspberry Pi 3. That is the first audio
this port has produced.

What it took, in the order the defects were actually found -- the last one was
the one that mattered, and the first three were necessary but could not have
been enough on their own:

1. `MAI_THR` from the SoC table was the BCM2711 value; BCM283x wants
   `0x08080608` (Linux vc4 uses the same figure);
2. `MAI_SMP` was computed from an `hsm_clock` that is ~9% wrong, and is now
   scaled from the value the legacy driver proved;
3. the IEC958 framing was being written in software on top of the framing the
   MAI block writes in hardware;
4. **the DMA path could not move a byte** -- virtual addresses handed to the
   engine untranslated, and no control block field byte-swapped.

A tree sweep confirms `sdcard` and `vcgfx`, the other two DMA users on this
port, always did (4) correctly. The three AHI drivers copied from arm-native
were the only offenders.

## Still open, separately

- **PWM and I2S are unverified.** They carried the same two DMA defects and are
  fixed the same way, but neither has been heard. PWM previously produced only
  a beep, which was `pwm_ramp_dc()` writing `PWM_DAT` from the CPU -- its DMA
  path has never run. The instrumentation now prints `DRAINING`/`STALLED` with
  `TXFR_LEN`, which settles it in one boot.
- The bring-up instrumentation in all three drivers is unconditional `bug()`.
  It should come down to `D()` once PWM and I2S are confirmed too.
