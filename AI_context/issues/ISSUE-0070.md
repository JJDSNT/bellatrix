---
id: ISSUE-0070
title: "Bellatrix AHI audio drivers: advertise what the hardware and the sink actually support"
status: open
priority: medium
type: research
owner: unassigned
created_at: 2026-08-29
updated_at: 2026-08-29
tags:
  - audio
  - ahi
  - hdmi
  - raspberry-pi-3
  - drivers
blockers: []
related_files:
  - aros/workbench/devs/AHI/Drivers/hdmiaudio/rpihdmi-main.c
  - aros/workbench/devs/AHI/Drivers/hdmiaudio/rpihdmi-hwaccess.c
  - aros/workbench/devs/AHI/Drivers/pwmaudio/
  - aros/workbench/devs/AHI/Drivers/i2saudio/
---

# Purpose

Determine the actual audio capabilities of the Raspberry Pi 3B and expose them
correctly through AHI. **This is not Paula or Rigel work** -- it is deliberately
separated from ISSUE-0071, which decides how Rigel's Paula connects to whatever
this produces. This issue can be worked now, with no Rigel involved, and doing
it first stops decisions taken for Paula from degrading the HDMI/PWM drivers.

The governing principle:

> The AHI driver should advertise only capabilities that are actually supported
> by the hardware, the connected output device, and the Bellatrix
> implementation.

# What is actually in the tree

Names first, because the natural ones are wrong. Upstream AROS has
`workbench/devs/AHI/Drivers/{RPiHDMI,RPiPWM,RPiI2S}`. Bellatrix has **forks of
all three**, renamed, under `aros/workbench/devs/AHI/Drivers/`:

| ours | upstream it came from | state |
| --- | --- | --- |
| `hdmiaudio/` (`HDMIAUDIO.s`) | `RPiHDMI/` | already diverged -- `rpihdmi-hwaccess.c` is 19.5 KB against upstream's 9.9 KB |
| `pwmaudio/` (`PWMAUDIO.s`) | `RPiPWM/` | present, `rpipwm-hwaccess.c` 27.9 KB |
| `i2saudio/` (`I2SAUDIO.s`) | `RPiI2S/` | present, smaller |

So PWM and I2S are not future backends to be written -- they exist and need
auditing, which is a cheaper starting position than the plan assumed. The file
names inside all three still say `rpihdmi-*`, `rpipwm-*`, `rpii2s-*`.

**What `hdmiaudio` advertises today**, from `rpihdmi-main.c`:

```c
static const LONG frequencies[] = { 8000, 11025, 22050, 44100, 48000 };

case AHIDB_Bits:        return 16;
case AHIDB_Frequencies: return FREQUENCIES;
case AHIDB_Frequency:   return (LONG) frequencies[argument];
case AHIDB_Outputs:     return RPiHDMIBase->num_outputs;
```

A hardcoded table and a hardcoded 16 bits. Nothing consults the sink, and
nothing verifies that a requested rate is the rate that leaves the connector.
That is the starting point for everything below.

# Architectural boundary

```text
applications -> ahi.device -> AHI mixer -> AHIsub driver -> Pi hardware
```

and for HDMI specifically:

```text
AHI -> hdmiaudio -> DMA -> HDMI MAI -> HDMI sink
```

The AHIsub driver is primarily responsible for **transporting the final audio
stream to the hardware**. Mixing of normal AHI voices stays an AHI
responsibility wherever the driver advertises software mixing support.

## AHI voices are not physical channels

```text
32 AHI voices -> AHI mixer -> stereo PCM (L+R) -> hdmiaudio
```

Bellatrix must **not** limit an AudioMode to four AHI voices merely because
Paula has four DMA channels. The mixer voice count is a performance decision,
not a Paula decision. Worth benchmarking: 8, 16, 32, and more if it is cheap
enough on Emu68.

# HDMI capabilities to investigate

## Sample rates

32000, 44100, 48000, 88200, 96000, 176400, 192000.

Do not expose a rate through AHI merely because the HDMI specification allows
it. For every advertised rate verify that MAI is programmed correctly, the
audio clock is correct, DMA pacing is correct, samples are not silently
resampled, playback duration is correct, pitch is correct, and the connected
sink accepts the mode.

First quality target: **44100 and 48000**. Then 88200 and 96000. Higher rates
only after validation.

## PCM resolution

16-bit and 24-bit. The implementation must distinguish actual sample precision,
storage/container width, DMA word format, and HDMI MAI sample representation.
Do not advertise 24-bit merely because samples can be stored in a 32-bit word;
verify that the path actually transmits the precision.

## Physical channel count

Prioritise 2-channel LPCM. Multichannel (2.0/5.1/7.1) is a later feature, and
before implementing it, check whether the AHI/AHIsub interfaces AROS uses can
represent those output formats cleanly at all.

# EDID capability discovery

The driver should ask the connected sink rather than assume.

```text
HDMI sink -> EDID -> Bellatrix EDID parser
                         |
                         +-- video capabilities
                         +-- audio capabilities -> hdmiaudio -> valid AudioModes
```

The CTA/CEA extension carries LPCM availability, maximum LPCM channel count,
supported sample rates and sample sizes. The Emu68 ecosystem has an `Emu68EDID`
utility worth studying as a reference for retrieving EDID on this hardware.

Determine: how Emu68 obtains the EDID; whether it uses the VideoCore
mailbox/property interface; whether equivalent infrastructure already exists in
Bellatrix or AROS; and whether EDID retrieval should be a shared Bellatrix
facility rather than HDMI-audio-specific code. **Do not duplicate EDID parsing
in the video and audio drivers** if one implementation is practical -- and note
that `vcgfx` already talks to this hardware, so the question is live rather
than hypothetical.

# AudioMode advertisement

Advertise the intersection:

```text
Pi hardware capability  n  hdmiaudio implementation  n  sink EDID
```

Conservative first target: stereo / 16-bit / 44100 and 48000. Then, after
validation: stereo / 24-bit at 44100, 48000, 96000. Exact modes come from
implementation and hardware testing, not from this list.

# Clock accuracy

For each supported rate verify that `requested rate -> actual MAI/DMA clock ->
actual samples per second` matches within tolerance, and in particular test the
44.1 kHz and 48 kHz families against each other. **A driver must not advertise
44.1 kHz while transmitting at 48 kHz or relying on unintended resampling.**

Validate with a known-frequency sine wave, an exact-duration sample, DMA
counter/timing measurements, and HDMI receiver information where available.

# DMA and buffering

Review the current strategy: buffer size, number of buffers, interrupt
frequency, underrun handling, cache coherency, DMA alignment, latency, CPU
overhead, interaction with the AHI mixer.

The objective is not minimum latency at any cost. It is **stable continuous
playback with reasonable latency and low CPU overhead.**

# PWM analog

The 3B's analog jack is PWM, not a dedicated DAC.

```text
AHI -> pwmaudio -> PWM hardware -> analog filter -> 3.5 mm
```

Conservative initial targets: stereo, 44100 and 48000, 16-bit PCM input. The
driver must not imply that the physical PWM output has the effective resolution
of a conventional 16-bit DAC. Investigate the PWM clock, effective resolution,
the Raspberry Pi high-quality PWM audio technique, dithering, noise shaping,
filtering and DMA feeding.

# PCM / I2S

```text
AHI -> i2saudio -> PCM/I2S -> external DAC / codec
```

Secondary unless there is an immediate hardware use case. It matters mainly
that the AHI architecture does not assume HDMI specifically.

# Units and physical outputs

AHI units may be configured for different AudioModes and backends. Eventually
something like unit 0 -> HDMI, unit 1 -> PWM analog, unit 2 -> I2S, unit 3 ->
USB audio. **This is configuration, not a Bellatrix ABI**: nothing may assume
that unit 0 means HDMI.

# Multiple AHIAudioCtrl instances

Test explicitly what `hdmiaudio` does with more than one simultaneous
`AHIAudioCtrl`. The driver appears to associate substantial state, including
DMA state, with each one, so several instances targeting the same MAI hardware
may compete for the same peripheral.

**Do not redesign this for Rigel.** Decide independently whether multi-client
AHIsub support is useful for Bellatrix at all. If it is implemented, hardware
ownership must become explicit: many logical clients -> shared physical HDMI
state -> a single MAI/DMA output. It is not required for the driver-quality
work here.

# Validation matrix

For every AudioMode eventually exposed:

```text
[ ] playback starts correctly        [ ] correct sample precision
[ ] playback stops correctly         [ ] no periodic glitches
[ ] correct channel orientation      [ ] no DMA underruns
[ ] correct sample rate              [ ] repeated open/close works
[ ] correct pitch                    [ ] application switching works
[ ] correct playback duration        [ ] multiple AHI voices work
[ ] CPU usage is acceptable          [ ] long-duration playback stays stable
```

Test at 1, 4, 8, 16 and 32 voices where supported.

# Order

1. Audit the current `hdmiaudio` AudioModes (started above: hardcoded
   8k/11.025k/22.05k/44.1k/48k and a hardcoded 16 bits)
2. Verify actual sample clocks
3. Verify 44.1 and 48 kHz
4. Verify PCM format and bit depth
5. Improve DMA/buffering if necessary
6. Increase and test the AHI mixer voice count
7. Add EDID retrieval
8. Derive supported modes from EDID
9. Validate 24-bit
10. Validate 88.2 / 96 kHz
11. Consider higher rates
12. Consider HDMI multichannel
13. Audit the existing `pwmaudio`
14. Audit the existing `i2saudio`

The goal is correctness and quality, not maximising the advertised
specification.
