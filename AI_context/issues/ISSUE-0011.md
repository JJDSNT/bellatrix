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
7. Then evaluate jack/PWM.
8. Then evaluate Bluetooth A2DP.

## Files to revisit

- `src/host/raspi3/vc_mailbox.c` (existing mailbox plumbing — what the
  HDMI *video* path already does via VideoCore; useful for clocks/firmware
  queries, not a full audio path by itself)
- `src/audio/output.c` / `output.h` (fixed-rate PCM producer for physical
  output)
- `src/audio/mixer.c` / `mixer.h` (HBLANK diagnostic/intermediate queue, not
  final sink PCM)
- ISSUE-0010 (depends on this only for the final target rate)
