// AI_context/issue_paula_audio_timing_and_simd.md

# Issue: Paula audio — timing instrumentation first, NEON/SIMD backend second

## Status: open

## Background

Paula audio has three conceptually distinct stages, with very different
SIMD payoff:

1. **Per-channel DMA emulation** (AUD0-3: period countdown, sample fetch,
   pointer/length reload, loop, IRQ) — a small state machine full of
   temporal dependencies. Low SIMD gain; this should stay scalar.
2. **Mixing** 4 channels → stereo (`L = ch0+ch3`, `R = ch1+ch2` or
   equivalent) — moderate, classic SIMD-friendly loop.
3. **Resampling / format conversion** (Paula's native rate, ~28 kHz-ish
   variable period, → 44.1/48 kHz host output) — the largest expected SIMD
   win; in other emulators this stage often dominates audio CPU time, more
   than the channel emulation itself.

The governing rule for this issue: **NEON/SIMD cannot fix timing bugs.** If
Paula's audio is wrong (sped up, slowed, clicking, looping at the wrong
point), the bug is upstream of any buffer — in AUDxPER calculation, sample
fetch scheduling, AUDxLC/AUDxLEN reload, AUDxDAT timing, AUDx INTREQ
generation, or Paula↔beam/CCK↔CPU sync. SIMD only makes a *correct* buffer
mix/resample faster. So: scalar-correct Paula first, NEON backend after.

## Current state (verified in code)

- `external/rigel/src/chipset/paula/audio.c` (286 lines) implements the
  AUD0-3 channel state machine: `audio_write_per()`, period countdown,
  `audio_channel_dma_enabled()`, DMA word fetch, IRQ bits 7-10 → INTREQ.
  This is the scalar core from item 1 above, and it already exists.
- The only sample consumer on the Bellatrix side is
  `bellatrix_machine_audio_left()/_right()` (`src/machine/machine_rigel.c:441-459`),
  which read **one mixed L/R sample at a time** via `rigel_get_audio_sample()`.
- **Nothing calls these two functions anywhere in the tree.** There is
  currently no audio output path at all: no buffer, no host audio driver
  (no I2S/HDMI code under `src/host/`), no resampler.
- `src/audio/mixer.c`, `mixer.h`, `synth.c`, `synth.h`, `midi_synth.h` are
  all 0-byte placeholder files — never filled in. This is clearly where the
  Bellatrix-side mixer/resampler backend was meant to land.
- A pre-Rigel note in `AI_context/consolidated/issue_paula_serial_floppy.md`
  already flagged two gaps that are still open today, just relocated inside
  Rigel: audio DMA is not a real arbitrated DMA requester (still just a
  direct step call), and audio DMA timing has never been validated against
  real hardware.

Net: there is no audio bug to chase yet in the "wrong sound" sense, because
nothing is listening to the output. The immediate problem is **timing
correctness has no way to be observed**, since the channel state machine
runs with zero visibility today.

## Part 1 — Timing instrumentation (do this first)

Add cycle-stamped trace points in `external/rigel/src/chipset/paula/audio.c`
(or its MMIO write path) for, per channel:

```
AUDxPER write
AUDxLEN / AUDxLC reload
DMA sample fetch (word ready)
period tick / countdown reload
loop / reload event
AUDxDAT write
INTREQ AUDx bit set
```

Timestamps must be in **chip cycles (CCK)**, never host wall-clock — this
follows the project's "chipset owns observable time" rule
(`CLAUDE.md` Architectural Principles). The goal is to be able to answer
"did this channel's period/reload/IRQ happen at the cycle it should have"
before any buffer or backend exists.

Open question: should this reuse `CORE2_LOG`/`BELLATRIX_CORE_LOG`
(Rigel runs on Core 2 — see `[[issue_multicore_boundary_logging]]`), or get
its own flag? Per-sample-fetch logging at full DMA rate is likely to be
very high frequency (worse than the MMIO crossing flagged as iterative in
that issue) — needs a per-channel filter or rate limit, decided once real
trace volume is seen, not designed up front.

## Part 2 — NEON/SIMD backend (only after Part 1 is verified correct)

Target architecture from the discussion:

```
Rigel/Paula (scalar, hardware-faithful)
    -> intermediate buffer
    -> Bellatrix audio backend (new)
         -> NEON mixer (4ch -> stereo)
         -> NEON resampler (Paula rate -> 44.1/48 kHz)
    -> host output (I2S / HDMI — driver does not exist yet)
```

- Mixer and resampler should be implemented in the currently-empty
  `src/audio/mixer.c`/`mixer.h`.
- Keep Rigel/Paula scalar and portable; all acceleration stays in the
  Bellatrix backend, never inside Rigel.
- With only 4 channels, raw mixing gain may be modest — the resampler +
  format conversion stage is the one expected to actually matter
  (consistent with planar→framebuffer conversion being the other
  NEON-worthy subsystem in this project).

## Next steps

1. Add the 7 trace points listed in Part 1 to `audio.c`, CCK-timestamped.
2. Validate channel timing against a reference (datasheet timing tables, or
   a WinUAE/FS-UAE trace comparison) before touching any mixer code.
3. Implement the missing consumer: drain
   `bellatrix_machine_audio_left/right()` into an actual buffer once per
   audio period — this is the first real caller and doubles as the bring-up
   target for Part 1's instrumentation.
4. Only after #2/#3 are verified correct: implement the NEON mixer +
   resampler in `src/audio/mixer.c`, and decide the host output driver
   (I2S vs HDMI) before fixing the resampler's target rate.

## Files to revisit

- `external/rigel/src/chipset/paula/audio.c`
- `external/rigel/src/domains/audio/audio_domain.c`
- `external/rigel/include/rigel/rigel_audio.h`
- `src/machine/machine_rigel.c`
- `src/audio/mixer.c` / `mixer.h` (empty stubs — intended landing spot)
- `src/audio/synth.c` / `synth.h` (empty stubs)
- `AI_context/consolidated/issue_paula_serial_floppy.md` (historical context: audio DMA arbiter gap, untested on hardware)
- `AI_context/issue_multicore_boundary_logging.md` (logging convention to reuse/extend)
