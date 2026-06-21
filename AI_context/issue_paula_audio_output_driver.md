// AI_context/issue_paula_audio_output_driver.md

# Issue: Bare-metal audio output driver — I2S vs HDMI

## Status: open (deferred, not decided)

## Context

Bellatrix has no host audio output driver at all today (no I2S, no HDMI
code under `src/host/`). A 2048-sample stereo ring buffer
(`src/audio/mixer.c`/`mixer.h`) is being filled from Paula's mixed output
at ~15 kHz (see `AI_context/consolidated/issue_paula_audio_timing.md`), but
nothing drains it yet — the NEON mixer/resampler that will
(`AI_context/issue_paula_audio_neon_mixer.md`) doesn't need this decision
made first, only the *final* target sample rate and where the bytes
actually go once a driver exists.

## The decision and why it's deferred

Asked to choose between I2S and HDMI; HDMI was picked first, then
researched before committing to implementation. Findings:

- **Bare-metal HDMI audio requires VCHIQ** — the VideoCore's RPC channel —
  because the GPU firmware does the actual PCM-into-HDMI-stream mixing,
  not the ARM core. There is no register-level path that bypasses this.
- **VCHIQ has no public specification.** The few known bare-metal
  implementations (e.g. Ultibo) are 10,000+ lines, ported from Linux kernel
  driver internals rather than from a documented protocol.
- Multiple multi-year-old Raspberry Pi forum threads from experienced
  bare-metal developers asking this exact question never reached a
  complete, working solution.
- Checked upstream Emu68 (`emu68/`, this project's CPU/JIT/display base —
  the codebase most likely to already have solved this, since it already
  does bare-metal HDMI *video* via VideoCore mailbox) for any existing
  audio or VCHIQ code to port from: there is none. Nothing to reuse.
- **I2S** (the BCM2837's PCM/I2S peripheral) is the documented,
  register-level alternative — same effort tier as this project's existing
  CIA 8520 / USB / Bluetooth work, no firmware RPC dependency.

Given that gap, the decision was **deferred rather than forced** — there's
no blocker to revisiting it later, and committing to HDMI now would mean
committing to an open-ended, possibly multi-week VCHIQ reverse-engineering
effort with no guaranteed outcome, based on threads where other
experienced bare-metal developers gave up.

Sources consulted:
- [How to way bare metal HDMI Audio?](https://www.raspberrypi.org/forums/viewtopic.php?f=72&t=147423)
- [bare metal hdmi audio](https://forums.raspberrypi.com/viewtopic.php?t=306441)
- [Bare metal HDMI audio programming](https://forums.raspberrypi.com/viewtopic.php?f=72&t=80750)

## What would need to happen to actually decide

- If I2S: confirm whether output goes to the 3.5mm analog jack (PWM, not
  I2S — would need a separate decision) or to an external I2S DAC/HDMI
  bridge board, since the Pi 3 doesn't expose I2S-over-HDMI directly on
  its own. Register-level I2S/PCM peripheral programming has no firmware
  dependency either way.
- If HDMI: scope a VCHIQ spike first — e.g. "can we get the firmware to
  pass through audio with zero VCHIQ code, by relying on whatever default
  HDMI audio config the firmware already negotiates," one of the
  approaches floated in the forum threads — before committing to writing
  a VCHIQ stack from scratch.
- Either way: the decision only needs to land before the NEON
  resampler's target rate is fixed (44.1 kHz vs 48 kHz vs something else)
  and before there's a real consumer for `audio_mixer_pop()`.

## Files to revisit

- `src/host/raspi3/vc_mailbox.c` (existing mailbox plumbing — what the
  HDMI *video* path already does via VideoCore; not sufficient for audio
  on its own, see above)
- `src/audio/mixer.c` / `mixer.h` (the ring buffer a driver would drain)
- `AI_context/issue_paula_audio_neon_mixer.md` (depends on this only for
  the final target rate, not blocked by it otherwise)
