---
id: ISSUE-0011
title: "Bare-metal audio output driver — HDMI/jack/Bluetooth"
status: doing
priority: medium
type: feature
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - audio
  - hdmi
  - baremetal
  - raspberry-pi
  - iec958
related_files:
  - src/audio/output.c
  - src/audio/output.h
  - src/audio/mixer.c
  - src/host/raspi3/vc_mailbox.c
---

# Issue: Bare-metal audio output driver — HDMI / jack / Bluetooth

## Status: open (bring-up started)

## Context

Bellatrix has no host audio output driver at all today (no I2S, no HDMI
code under `src/host/`). A 2048-sample stereo ring buffer
(`src/audio/mixer.c`/`mixer.h`) is being filled from Paula's mixed output
at ~15 kHz (see `AI_context/consolidated/issue_paula_audio_timing.md`), but
nothing drains it yet — the NEON mixer/resampler that will
(ISSUE-0010) doesn't need this decision made first, only the *final* target
sample rate and where the bytes actually go once a driver exists.

Important update from the harness audio work: the HBLANK-fed queue is **not**
the final PCM output stream. It is a useful diagnostic/intermediate queue, but
HBLANK cadence is about 15 kHz on PAL and does not match HDMI/jack/Bluetooth
sink rates. The harness path that now sounds substantially better generates
host PCM by sampling `bellatrix_machine_audio_left/right()` at a fixed output
rate. The bare-metal bring-up must do the same: produce a steady 44.1/48 kHz
PCM stream first, then feed a physical sink.

## Current implementation seed

`src/audio/output.c` / `output.h` now provide the first bare-metal output
layer:

- target rate: 48 kHz, chosen as the first HDMI bring-up rate;
- source: `bellatrix_machine_audio_left/right()`;
- timing: fractional accumulator over the current Rigel clock via
  `rigel_get_clock_hz()`; this must support PAL, NTSC and any future custom
  machine clock rather than baking in `7093790 Hz`;
- storage: independent 4096-frame stereo S16 ring buffer;
- stats: produced, consumed, dropped and underrun samples.

This layer is deliberately separate from `AudioMixerQueue`. The next HDMI,
jack or Bluetooth backend should consume `bellatrix_audio_output_pop()` rather
than the HBLANK queue.

## HDMI plan and VCHIQ

Asked to choose between I2S and HDMI; HDMI was picked first, then
researched before committing to implementation. Earlier notes assumed HDMI
audio required VCHIQ/firmware. That is still a possible route, but it is not
the only route worth investigating.

- **Route A: direct HDMI audio block / IEC958 framing.** Circle has a
  `CHDMISoundBaseDevice` driver and lists "HDMI sound output (without VCHIQ)"
  as a supported bare-metal feature. This is now the preferred first spike:
  port the minimal HDMI register/FIFO/audio-infoframe/IEC958 path needed for
  a 48 kHz stereo tone, then feed it from `src/audio/output.c`.
- **Route B: VCHIQ firmware audio service.** VCHIQ is still relevant, but it
  should be treated as a fallback or separate spike. It is the VideoCore RPC
  path used by firmware services; public documentation is limited, and known
  bare-metal implementations tend to be large ports from Linux/firmware
  internals rather than small register-level drivers.
- **Route C: jack/PWM or I2S.** Useful after the PCM producer is stable, but
  it answers a different hardware path than HDMI.
- **Route D: Bluetooth A2DP.** Last, because it adds SBC encoding, remote
  clocking, buffering and pairing/state complexity on top of the PCM pipeline.

VCHIQ comments:

- It may be attractive if the firmware can own all HDMI audio details, but it
  adds a large protocol surface before we even know the Bellatrix PCM producer
  is correct on metal.
- It is a poor first proof because underruns/clicks could be caused by Paula
  timing, our PCM producer, the firmware service, VCHIQ scheduling, or HDMI
  sink negotiation.
- Keep it documented as a possible later route, especially if the direct
  HDMI/IEC958 path proves Pi-model-specific or too invasive.

Previous findings that still matter:

- Multiple multi-year-old Raspberry Pi forum threads from experienced
  bare-metal developers asking this exact question never reached a
  complete, working solution.
- Checked upstream Emu68 for any existing audio or VCHIQ code to port from:
  there is none. Nothing to reuse there.
- I2S/PWM remain documented, register-level alternatives for non-HDMI sinks.

## Bring-up sequence

1. Validate `src/audio/output.c` on metal with counters: produced samples
   should track 48 kHz over wall time; queue should overflow if no backend
   drains it.
2. Add a test-tone source option before Paula, so the physical sink can be
   debugged without Amiga timing in the loop.
3. Port the minimal Circle-style HDMI path: HDMI reset/config, audio infoframe,
   IEC958 sample conversion and FIFO write in polling mode.
4. Play a 48 kHz stereo tone over HDMI.
5. Drain `bellatrix_audio_output_pop()` into HDMI and test Paula audio.
6. Only after HDMI proof: add DMA/ring refill if polling is too expensive.
   IRQ-driven drain (MAI FIFO threshold interrupt via the BCM2837 interrupt
   controller, same style as the existing Bluetooth/PL011 IRQ paths) is an
   alternative or companion to DMA here — not yet implemented, not yet
   scheduled; register today only masks nothing (`MAI_CTL` has no IRQ-enable
   bits set), so the driver is 100% polling from the chipset step loop.
7. Then evaluate jack/PWM.
8. Then evaluate Bluetooth A2DP.

## Implementation status (2026-07-03)

Steps 2–5 implemented: `src/host/raspi3/hdmi_audio.h`/`.c` (register-level
MAI/HDMI driver, IEC958 framing, N/CTS via from-scratch continued-fraction
rational approximation — independent reimplementation from BCM2835/2837
register facts and the published IEC 60958-3/CEA-861 standards, not a port
of Circle's GPL-3 `CHDMISoundBaseDevice`, since Bellatrix intends to be
MIT). Wired: `hdmi_audio_init()` called from `src/cpu/emu68/bellatrix.c`
(next to Bluetooth bring-up); drain loop in `src/machine/machine_rigel_step.c`
under `#ifndef BELLATRIX_HARNESS`, pulling from
`bellatrix_audio_output_pop()` whenever both the HDMI FIFO is writable and
the queue is non-empty. Added to `cmake/bellatrix-variant.cmake`; confirmed
absent from the harness build (harness is unaffected).

Verified: `hdmi_audio.c` compiles cleanly for bare-metal
(`BELLATRIX_CPU_BACKEND=musashi`). Final link currently fails only on the
pre-existing, unrelated ISSUE-0035 (`getenv`/`strtoul` gap in
`ata_ide.c`/`atapi_cdrom.c`/blitter code) — not a regression from this work.

**Not yet verified**: actual sound over a physical HDMI sink. Requires
flashing a real Pi 3B with an HDMI display/soundbar attached — needs the
user to test on hardware; `kprintf` diagnostics are left in place at
`hdmi_audio_init()` (HSM clock read, computed N/CTS) to aid that.

## Regression found: QEMU boot hang (2026-07-03)

Calling `hdmi_audio_init()` unconditionally from `bellatrix_init()` broke
QEMU boot (`./run.sh qemu`) while the harness kept working — the harness
never builds/calls this code at all, so it masked the problem. Root cause:
QEMU's `raspi3b` machine model does not implement the HDMI/MAI/Clock
Manager MMIO block (`ARM_HD_BASE`/`ARM_HDMI_BASE`/`CM_HSMCTL` etc.); an
access to unmodeled peripheral MMIO there hangs the guest before Kickstart
even runs.

Fix: added `BELLATRIX_ENABLE_HDMI_AUDIO` CMake option (default **OFF**,
same convention as `BELLATRIX_ENABLE_BTSTACK`/`_USBSTACK`) gating both the
`hdmi_audio_init()` call in `bellatrix.c` and the drain loop in
`machine_rigel_step.c`. Only real bare-metal images built specifically to
test HDMI audio on physical Pi 3B hardware should turn this on
(`-DBELLATRIX_ENABLE_HDMI_AUDIO=ON` via `scripts/build.sh`, needs a build.sh
env-var passthrough added if not already wired — check before relying on
it). QEMU and default builds are unaffected now.

## Cross-check vs kumaashi reference + real-HW result (2026-07-09)

Added `external/RaspberryPI` (kumaashi, git submodule) as a hardware-fact
reference. The **Pi Zero W (BCM2835)** samples are the correct match for the
Pi 3B (BCM2837) — same VideoCore4 HDMI/MAI block; the `RPI4/` samples are a
different HDMI controller and were ignored. No LICENSE in that repo → used only
as a register/hardware-fact reference (clean-room, same stance as the existing
Circle-avoidance), never a code port.

**Confirmed-working reference:** `RPIZEROW/Sample_HDMI_DMA_Audio_03` (plays
`SevillaAlbeniz.wav` via DMA+DREQ; YouTube demo `Fy4gApu8K_s`; user confirmed it
works). Identical register sequence to `Sample_HDMI_DMA_Audio_Streaming_01`.

Fixes applied this session to `src/host/raspi3/hdmi_audio.c`:
- HDMI_BASE audio register offsets corrected: AUDIO_PACKET_CONFIG 0x09c, CRP_CFG
  0x0a8, CTS_0 0x0ac, CTS_1 0x0b0 (were 0xBC/0xC0/0xC4/0xC8; 0xC0 collided with
  HDMI_SCHEDULER_CONTROL). MAI block (HD_BASE) offsets were already correct.
- CTS_0 and CTS_1 now both get the full 20-bit CTS (were split low/high). N no
  longer overwritten by the CTS math. Removed the wrong-offset TX_PHY poke
  (firmware owns the PHY via Emu68 init_display mailbox). Added a post-init
  register dump (MAI_CTL/MAI_FMT/APCFG/RAMPKT_CFG/RAMPKT_STS) for HW capture.
- `config.txt` via **patch 0009**: re-added `hdmi_drive=2` (+ `hdmi_stream_channels=1`,
  `no_hdmi_resample=1`) — was removed 2026-07-04 for a serial-silence diagnostic;
  without it the firmware stays in DVI mode and carries no audio.

**Real-HW result (2026-07-09): still NO sound.** Only relevant serial line:
`hdmi_audio: HSM clock read as 0 Hz, cannot derive N/CTS` → `hdmi_audio_init()`
**bails out before configuring anything** (returns false, s_hdmi_ready stays 0).
This blocker sits *upstream* of all the register-value bugs below.

Root cause of the bail: `hdmi_read_hsm_clock_hz()` reads CM_HSMCTL/CM_HSMDIV
(CM_BASE 0x101000 + 0xB0/0xB4) and gets 0 (int_div==0). **This whole HSM-clock
derivation is our own invention (Linux vc4 style). The proven kumaashi reference
never reads the CM/HSM clock at all** — it hardcodes N=6144, CTS_0=CTS_1=0x1220A,
MAI_SMP=0x0DCD21F3. Recommended fix: drop the hard-fail and hardcode N/CTS/SMP
like the reference (48 kHz), removing the CM dependency entirely.

Remaining register-VALUE divergences vs the proven `_03` sequence (only matter
once init stops bailing):
- MAI_FMT: proven 0x20900 (2ch/48k) — ours 0
- MAI_CONFIG: proven (1<<27)|(1<<26)|(1<<1)|(1<<0) — ours 0
- MAI_CHANNEL_MAP: proven 0x8 — ours 0x10
- MAI_THR: proven 0x08080608 — ours 0x1010
- AUDIO_PACKET_CONFIG: proven (1<<29)|(1<<24)|(1<<1)|(1<<0) — ours (1<<31) (wrong;
  there is no bit-31 "enable" — enable is the L/R channel bits)
- MAI_CTL (prepare): proven bit3|(ch<<4)|bit12|bit13 — ours a different layout
- Audio infoframe: proven → RAM packet slots 4 & 5 (words 0x000A0184, 0x00000170),
  enabled via RAM_PACKET_CONFIG bit16 + bit4 — ours → slot 0, never enabled
- Sample word format: proven raw `(u16 sample)<<16>>4 & ~0xF` (bits[27:12]), HW
  does the IEC958 framing — ours builds full IEC958 subframes in software
- FIFO feed: proven DMA+DREQ (DMA_PERMAP_HDMI, dest bus addr HD_BUS_BASE
  0x7E808000) — ours CPU polling from the chipset step loop (kumaashi never
  demonstrated a polling feed working; that path is unproven)

Open decision (deferred): after init proceeds, align the register values, then
choose the output path — keep polling (smaller, unproven) vs port the proven
DMA+DREQ path (bigger: BCM DMA engine, cache-coherent buffers, bus addresses,
refill IRQ = step 6 above). See memory `bellatrix-hdmi-audio-findings`.

## DMA feed implemented + real-HW results (2026-07-09, part 2)

Implemented the IRQ-free DMA+DREQ feed in `src/host/raspi3/hdmi_audio.c`
(replaces the polling MAI_DATA writes; `hdmi_audio_dma_poll()` called from
`machine_rigel_step.c`):
- Legacy BCM DMA controller (peripheral +0x7000), **channel 5** — confirmed the
  controller is unused by Emu68 (only DWC2's own USB DMA) and Bellatrix (EMMC is
  PIO), so a full channel is free; **no interrupts** (self-relinking 2-CB ring, we
  poll DMA_SOURCE_AD and refill the half the DMA just left). This sidesteps the
  Emu68-owns-Pi-setup concern entirely.
- Double buffer, dest = MAI FIFO bus addr 0x7E808020, src = `0xC0000000 | phys`
  (Pi3 uncached VC alias), PERMAP=17 (HDMI) + DEST_DREQ. Build is
  **elf64-bigaarch64**, so all DMA-read RAM (control blocks + sample words) is
  stored via `CPU_TO_LE32`; `arm_flush_cache` after each fill. Uses Emu68
  `support.h`/`mmu.h` (`arm_flush_cache`, `mmu_virt2phys`), same as the USB glue.

Real-HW result: **the DMA path works — audio reaches the speaker.** Findings:

1. **Clock-domain starvation (real Paula feed).** [HDMI-AUD] diag: underrun grows
   ~50k/report vs consumed ~2k/report → ~96% of the hardware's real-time 48 kHz
   demand is filled with silence. The MAI consumes at real-time 48 kHz; the Amiga
   under **Musashi (interpreter) runs ~1/25 real-time**, so production can't keep
   up → hiss. Fundamental: a fixed-rate real DAC needs ~real-time production;
   correct HDMI audio is gated on a full-speed (JIT) emulator, not Musashi. Not
   fixable by resampling (the audio itself is generated slowly).

2. **Broadband/PCM hiss root-caused: `HDMI_MAI_CONFIG`.** Debug sequence:
   - `HDMI_DMA_TEST_MODE=SINE` produced clean tones.
   - `HDMI_DMA_TEST_MODE=CHIRP` produced the expected sweeping "UFO" sound,
     proving DMA/DREQ/ring/cache/MAI FIFO transport were alive.
   - A real PCM clip (`SevillaAlbeniz.wav`) still produced white noise even after
     regenerating `hdmi_clip.h` from the WAV with a proper RIFF parser, testing
     mono/attenuated output, trying 720p60/CTS matching, and finally embedding the
     WAV with `.incbin` to mimic the reference exactly.
   - Forcing the **entire** Pi Zero reference HDMI packet setup made the sink
     silent because the Pi 3B firmware-negotiated packet/channel state differs:
     firmware PRE shows `CHMAP=00fac688`, `APCFG=21000403`,
     `RAMPKT_CFG=00010015`.
   - The working fix is the hybrid state: keep firmware `CHMAP/APCFG/RAMPKT_CFG`,
     but override only `HDMI_MAI_CONFIG` to the reference value
     `(1<<27)|(1<<26)|(1<<1)|(1<<0) == 0x0c000003`.
   - Keep `mai_word()` in the proven raw format (`sample<<16>>4 & ~0xf`,
     sample bits `[27:12]`). The alternative `sample<<16` slot test was not the
     answer.

3. **Production mode restored.** After validating the clip path, `HDMI_DMA_TEST_MODE`
   was returned to `QUEUE` (`0`) so the DMA ring drains Bellatrix's Paula output
   queue again. The debug ring was expanded from 2 halves to 8 segments
   (`HDMI_DMA_SEGMENTS=8`) and diagnostics now log `seg` plus `s0..s7` refill
   counters; balanced counters confirm normal ring health.

Merge note for `wip/emu68-public-api`: that branch also touches HDMI/audio build
plumbing. When merging it into `main`, preserve the working HDMI audio invariant:
firmware packet/channel state stays untouched, but `HDMI_MAI_CONFIG` must be
forced to `0x0c000003`; production default must remain `HDMI_DMA_TEST_MODE=0`.

## Files to revisit

- `src/host/raspi3/vc_mailbox.c` (existing mailbox plumbing — what the
  HDMI *video* path already does via VideoCore; useful for clocks/firmware
  queries, not a full audio path by itself)
- `src/audio/output.c` / `output.h` (fixed-rate PCM producer for physical
  output)
- `src/audio/mixer.c` / `mixer.h` (HBLANK diagnostic/intermediate queue, not
  final sink PCM)
- `src/host/raspi3/hdmi_audio.c` / `.h` (this session's polling-mode driver;
  IRQ/DMA drain is the next evolution, see step 6 above)
- ISSUE-0010 (depends on this only for the final target rate)
