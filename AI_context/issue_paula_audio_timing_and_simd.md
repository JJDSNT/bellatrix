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
AUDxPER write                   <- done, all 4 channels
AUDxLEN / AUDxLC reload          <- done, all 4 channels (RIGEL_LOG_EVENT_AUDIO_RELOAD)
DMA sample fetch (word ready)    <- done, all 4 channels (RIGEL_LOG_EVENT_AUDIO_FETCH)
period tick / countdown reload   <- done, all 4 channels
AUDxDAT write                    <- done, all 4 channels (RIGEL_LOG_EVENT_AUDIO_DAT_WRITE)
INTREQ AUDx bit set              <- done, all 4 channels
```

All 6 trace points from the original 7-bullet plan are implemented (the
7th, "loop/reload event", was the same event as AUDxLEN/AUDxLC reload —
there's no separate "loop" moment in this state machine beyond the reload
that happens when `current_length` hits zero and gets refilled).

Timestamps must be in **chip cycles (CCK)**, never host wall-clock — this
follows the project's "chipset owns observable time" rule
(`CLAUDE.md` Architectural Principles).

### Progress: full 6-event / 4-channel instrumentation — implemented and validated

The open question of which log flag to reuse turned out to have a cleaner
answer than expected: `core_log.h`/`BELLATRIX_CORE_LOG` doesn't apply here
at all, because Paula's audio state machine runs entirely **inside Rigel on
Core 2** — it's not a cross-core boundary (see
`[[issue_multicore_boundary_logging]]`'s "major correction": only
CPU↔Chipset and IO↔Chipset are real cross-core events). Rigel's own
architecture (`external/rigel/CLAUDE.md`: "host owns I/O") rules out calling
Bellatrix's `kprintf`/`core_log.h` macros directly from `audio.c` — that
would couple a portable, host-agnostic library to one host's debug macros.

Instead, reused Rigel's existing structured event mechanism
(`rigel_log_event()` / `rigel_log_event_fn_t`, in `include/rigel/rigel_config.h`
+ `src/debug/log.c`), already wired host-side via
`machine_rigel.c:207`'s `config.log_event_fn = machine_rigel_log_event` and
already gated by the same `g_rtrace.enabled`/`BELLATRIX_RIGEL_TRACE_BUILD`
flag as `[RIGEL-IPL]`/`[RIGEL-DMACON]` — no new flag needed. Added 6 new
event IDs (`RIGEL_LOG_EVENT_AUDIO_PER_WRITE/_PERIOD/_IRQ/_RELOAD/_FETCH/_DAT_WRITE`)
and emission sites in `audio.c`, each capped at its own 512-event counter
(matching the existing convention in `copper_exec.c`/`slot_scheduler.c`,
which protects Rigel's own native/test builds where the default sink is
`stderr`). The consumer (`machine_rigel_log_event()` in
`machine_rigel_trace.c`) prints `[RIGEL-AUDIO-PER]`/`-TICK`/`-IRQ`/`-RELOAD`/
`-FETCH`/`-DAT`, each stamped with `rigel_get_time(g_rigel)` read live at the
moment the event fires — more precise than the `r->time` end-of-step-batch
stamp `RigelTrace` uses elsewhere, since this reads the exact cycle of the
event itself.

Started with a channel-0-only, 3-event slice to validate the mechanism
cheaply, then ran it against a real Kickstart/AmigaOS boot
(`BELLATRIX_RIGEL_TRACE_BUILD=1`). The read was: a CIA-B/EXTER (level 6,
`vec=078` — **not** Paula's own audio IRQ, which is level 4/`vec=070`)
interrupt fires roughly once per frame, its ISR writes DMACON to
re-(assert) all 4 `AUDxEN` bits and updates channel 0's `AUDxPER`, and
exactly one `[RIGEL-AUDIO-TICK]` fires per burst — consistent with a
CIA-timer-driven music replayer retriggering a short one-shot sample each
tick, a normal Amiga programming idiom. `AUDxPER` changed sensibly across
bursts (538→269→214), suggesting period writes land correctly. Could not
confirm from 3 events alone whether "one tick per burst" meant a genuinely
tiny `AUDLEN` (correct) or channel starvation (bug) — that's what motivated
expanding to the remaining 3 event types and all 4 channels.

### Bug found in the instrumentation itself: 512-event cap exhausted within the first fraction of a second

After expanding to all 6 events/4 channels and re-running against a real
boot, two independent captures (frame ~800, cyc≈56-69M) showed `[RIGEL-AUDIO-PER]`
(the AUDxPER write) firing as expected, but **zero** `[RIGEL-AUDIO-TICK]`,
`-RELOAD`, `-FETCH`, `-IRQ`, or `-DAT` lines across the entire window — for
*any* of the 4 channels, not just channel 0.

Traced the call chain to confirm this wasn't a chipset wiring gap:
`rigel_chipset_step()` (`external/rigel/src/chipset/chipset.c:74-76`) calls
`rigel_paula_set_dmacon(&chipset->paula, chipset->agnus.dma.dmacon)` *and*
`rigel_paula_step()` (→ `audio_step()`) unconditionally on every single
chipset step — no gating, and `rigel_dma_apply_setclr()`
(`domains/dma/dma_domain.c`) preserves all of DMACON's bits including the
audio enables, so `audio_channel_dma_enabled()` should read true and the
period-tick loop should run continuously. The wiring is correct.

The real cause: `AUDIO_TRACE_LIMIT` was `512`, and it's a **single counter
shared across all 4 channels** per call site. With period ticks firing every
~200-900 CCK per channel from the moment boot starts, the shared budget for
`RIGEL_LOG_EVENT_AUDIO_PERIOD` (and likewise `_FETCH`) exhausts within
roughly the first 50,000-200,000 CCK of boot — a tiny fraction of the 56-69
million CCK window captured. The cap (copied from `copper_exec.c`/
`slot_scheduler.c`'s precedent, where it guards a narrow, late-triggered
condition) was the wrong tool for an event meant to be observed continuously
across a multi-frame boot. Raised `AUDIO_TRACE_LIMIT` to `1000000u` — large
enough to cover a multi-second bring-up capture without going dark, still
finite as a safety net. Rebuilt and re-validated (build, `test_audio`, QEMU
boot smoke) — no regression.

**Next capture should be taken from the very start of boot** (or at least
much earlier than frame ~800) to see `[RIGEL-AUDIO-RELOAD]`/`-FETCH` and
settle the original question (tiny intentional `AUDLEN` vs. starvation).

Validated: Rigel's own `test_audio` unit test (`ctest`/standalone binary)
exercises `audio_write_per()`/`audio_write_dat()` and prints all new events
correctly via the default stderr sink. Full Bellatrix build
(`BELLATRIX_MULTICORE_BUILD=1 BELLATRIX_LOGS=1 BELLATRIX_BTSTACK=1
BELLATRIX_USBSTACK=1`) — exit 0, `strings` on `Emu68.img` confirms all 6
format strings present, QEMU boot smoke test unaffected. (Pre-existing,
unrelated build break found in Rigel's own `test_copper.c` — wrong arg
count for `rigel_copper_domain_step` — not touched by this work, flagging
here so it isn't mistaken for something this change caused.)

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

1. ~~Re-capture from near the start of boot and check RELOAD/FETCH cadence.~~
   **Done — resolved, not a bug.** A capture taken after raising
   `AUDIO_TRACE_LIMIT` (cyc≈53.19M-53.98M) shows, for all 4 channels:
   `AUDLEN` reloads to a small fixed value (ch0=1, ch1-3=2 words), each word
   is consumed over exactly 2 period ticks (one word = 2 packed 8-bit
   samples, matching real Paula hardware), and `RELOAD`→`FETCH`→`IRQ` land
   on the same cycle whenever the reload immediately empties (`audlen=1`).
   Channel 0 retriggers continuously every ~454 CCK (≈2×214), channels 1-3
   every ~1712 CCK (≈2×856) — exactly proportional to `audlen×audper` in
   every case. This is a continuous one-shot retrigger of a short
   sample/test-tone across all 4 channels, not starvation. Cadence is
   internally consistent with the register values in every channel —
   the core goal of Part 1 (verify AUDLEN/AUDPER/fetch/IRQ timing
   correctness) is satisfied.
2. **Open, non-blocking residual**: every `[RIGEL-AUDIO-FETCH]` in that
   capture showed `word=0000`, across 3 different addresses
   (`0427f2`, `03e436`, `03e434`). Could be genuine early-boot silence
   (buffer not yet populated with real sample data) or could mean the
   `mem.read16` callback isn't returning real Chip RAM content at these
   addresses yet at this point in boot. Not investigated further — doesn't
   block the timing conclusion above (Part 1 is about *cadence*, not
   *content*), but worth a memory dump check at these addresses if audio
   content (not just timing) becomes the focus later.
3. Validate channel timing against an external reference (datasheet timing
   tables, or a WinUAE/FS-UAE trace comparison) — not done; the cadence
   self-consistency check in #1 is internal (AUDLEN/AUDPER agree with each
   other), not cross-checked against an independent source yet.
4. ~~Implement the missing consumer: drain
   `bellatrix_machine_audio_left/right()` into an actual buffer.~~
   **Done — push side validated against a real harness run.** Added
   `src/audio/mixer.c`/`mixer.h` (a 2048-sample stereo ring buffer,
   drop-oldest-on-full, matching the `RuntimeEventQueue` style in
   `runtime/event.c`), a `BellatrixMachine.audio_queue` field, and
   `bellatrix_machine_on_audio_sample_ready()` wired to
   `RIGEL_EVENT_HBLANK` (the rate `rigel_audio.h` documents as the natural
   tick for `rigel_get_audio_sample()`) in **both** `rigel_step()` call
   sites — `core_chipset.c` (multicore) and `machine_quantum_step()` in
   `machine_rigel_step.c` (single-core/harness; this path also had its own
   inline `FRAME_READY` handling instead of calling
   `bellatrix_machine_on_frame_ready()` — unified it, since that's also
   where the new per-frame `[RIGEL-AUDIO-QUEUE]` diagnostic lives).
   Verified end-to-end on the harness: `count=2048` (always full) and
   `dropped` growing by exactly 312/frame (= PAL scanlines = HBLANKs per
   frame) — internally consistent, confirms the push path is alive. No pop
   consumer exists yet by design (no host output driver), so the queue
   filling and discarding the oldest sample every push is the correct,
   expected steady state, not a bug.
5. Only after #3 is verified correct: implement the NEON mixer +
   resampler in `src/audio/mixer.c` (the ring buffer's pop side is what
   they'll drain from), and decide the host output driver (I2S vs HDMI)
   before fixing the resampler's target rate.

## Files to revisit

- `external/rigel/src/chipset/paula/audio.c` (6 trace points, all 4 channels)
- `external/rigel/include/rigel/rigel_config.h` (6 new `RIGEL_LOG_EVENT_AUDIO_*` IDs)
- `src/machine/machine_rigel_trace.c` (`machine_rigel_log_event()` — new cases added)
- `src/audio/mixer.c` / `mixer.h` (no longer empty — ring buffer implemented)
- `src/machine/machine.h` (`BellatrixMachine.audio_queue`,
  `bellatrix_machine_on_audio_sample_ready()` declaration)
- `src/machine/machine_rigel.c` (`audio_mixer_init()` in init/reset)
- `src/machine/machine_rigel_step.c` (`bellatrix_machine_on_audio_sample_ready()`
  definition; `machine_quantum_step()` HBLANK hook + FRAME_READY unification)
- `src/runtime/core_chipset.c` (HBLANK hook, multicore path)
- `cmake/bellatrix-variant.cmake` and `tools/harness/CMakeLists.txt`
  (both needed `src/audio/mixer.c` added to their source lists)
- `external/rigel/src/domains/audio/audio_domain.c`
- `src/machine/machine_rigel.c`
- `src/audio/mixer.c` / `mixer.h` (empty stubs — intended landing spot)
- `src/audio/synth.c` / `synth.h` (empty stubs)
- `AI_context/consolidated/issue_paula_serial_floppy.md` (historical context: audio DMA arbiter gap, untested on hardware)
- `AI_context/consolidated/issue_multicore_boundary_logging.md` (real cross-core boundaries vs intra-Rigel events)
- `AI_context/issue_core_log_vs_rigeltrace.md` (the core_log.h/RigelTrace duality this issue ended up confirming applies here too)
