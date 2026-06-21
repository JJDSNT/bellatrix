// AI_context/issue_paula_audio_neon_mixer.md

# Issue: Paula audio — NEON mixer/resampler backend

## Status: open

## Context

Full history of the timing-instrumentation work that cleared the way for
this is in `AI_context/consolidated/issue_paula_audio_timing.md`. Summary:
Paula's AUD0-3 timing (AUDLEN/AUDPER/fetch/IRQ cadence) is now instrumented
and validated as internally consistent across all 4 channels. A 2048-sample
stereo ring buffer (`src/audio/mixer.c`/`mixer.h`) is filled from
`RIGEL_EVENT_HBLANK` (~15 kHz) via `bellatrix_machine_on_audio_sample_ready()`
— validated end-to-end on a real harness run. No pop consumer exists yet.

Governing rule carried over: **NEON/SIMD cannot fix timing bugs** — only
write the mixer/resampler once the upstream timing is trusted, which the
consolidated work above established for cadence (not full content
correctness — see residual #1 below).

**Blocking prerequisite, found after this issue was first written**: Paula
audio is currently audibly choppy on the host (reported in the harness).
Root cause is believed to be CPU↔chipset stepping granularity at the point
samples are extracted, not anything this issue would touch — see
`AI_context/issue_paula_audio_cpu_chipset_sync.md`. Fix that first; a NEON
mixer/resampler built on top of samples extracted at the wrong moments
just makes wrong audio faster.

Where the final mixed/resampled output actually goes (I2S vs HDMI) is a
**separate, independent decision** — see
`AI_context/issue_paula_audio_output_driver.md`. This issue is just the
algorithm: draining the ring buffer, mixing, resampling. It doesn't need
the output driver resolved first, only the final target sample rate once
that's picked.

## Open items

1. **`word=0000` residual (non-blocking, but check before trusting content)**.
   Every `[RIGEL-AUDIO-FETCH]` captured so far shows `word=0000`, across
   multiple distinct addresses (`0427f2`, `03e436`, `03e434`). Could be
   genuine early-boot silence (buffer not yet populated with real sample
   data) or could mean `mem.read16` isn't returning real Chip RAM content
   at these addresses yet at this point in boot. Doesn't block the
   timing/cadence conclusion (that's about *when*, not *what*), but worth a
   memory dump check at these addresses before trusting the mixer's actual
   audio *content* — no point writing a careful NEON mixer for data that
   turns out to be silence due to a read-path bug rather than the song
   actually being quiet there.
2. **No external reference validation yet.** The cadence check so far is
   internal self-consistency (AUDLEN×AUDPER agrees with observed retrigger
   spacing) — not cross-checked against a datasheet timing table or a
   WinUAE/FS-UAE trace. Worth doing before treating the upstream timing as
   fully trustworthy, not just "internally plausible."
3. **Sandbox limitation, not a code issue**: this session's own QEMU runs
   never get past the bare-metal launcher's framebuffer logo screen (no
   SD card/input available in that sandbox), so the rich boot captures
   used to validate cadence and queue behavior all came from the user's
   own environment. Keep that in mind if picking this back up somewhere
   that also can't run a full interactive boot — captures need to come
   from a real session, not a headless smoke test.
4. **NEON mixer + resampler — not started.** Target architecture:

   ```
   Rigel/Paula (scalar, hardware-faithful)
       -> intermediate buffer (src/audio/mixer.c — exists)
       -> Bellatrix audio backend (new)
            -> NEON mixer (4ch -> stereo)
            -> NEON resampler (Paula rate -> 44.1/48 kHz)
       -> host output (driver TBD — see issue_paula_audio_output_driver.md)
   ```

   Lands in `src/audio/mixer.c`/`mixer.h`, draining from the ring buffer's
   pop side (`audio_mixer_pop()`, already implemented and unused so far).
   With only 4 channels, raw mixing gain may be modest — the resampler +
   format conversion stage is the one expected to matter (same shape as
   planar→framebuffer conversion, this project's other NEON-worthy
   subsystem).

## Suggested order

#1 and #2 are cheap sanity checks worth doing before sinking time into
NEON code that mixes/resamples data nobody's confirmed is right yet. #3 is
just an operating constraint, not a task. #4 is the real work.

## Files to revisit

- `src/audio/mixer.c` / `mixer.h` (ring buffer exists; mixer/resampler land here)
- `external/rigel/src/chipset/paula/audio.c` (trace points, if `word=0000` needs more digging)
- `src/machine/machine_rigel.c` (`rigel_get_audio_sample()` call site, chip RAM read callback wiring)
- `AI_context/consolidated/issue_paula_audio_timing.md` (full history)
- `AI_context/consolidated/issue_paula_serial_floppy.md` (historical: audio DMA arbiter gap, untested on hardware)
- `AI_context/issue_core_log_vs_rigeltrace.md` (related logging-duality question)
- `AI_context/issue_paula_audio_output_driver.md` (where the resampled output goes)
