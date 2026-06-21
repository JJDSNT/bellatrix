// AI_context/consolidated/issue_paula_audio_timing.md

# Issue: Paula audio — timing instrumentation and buffer consumer (Part 1)

## Status: resolved (2026-06-21)

## Origin

Paula audio has three conceptually distinct stages, with very different
SIMD payoff: per-channel DMA emulation (low gain, stays scalar), mixing 4
channels → stereo (moderate, SIMD-friendly), resampling/format conversion
to 44.1/48 kHz (largest expected win). Governing rule: **NEON/SIMD cannot
fix timing bugs** — if Paula's audio is wrong, the bug is upstream of any
buffer (AUDxPER calculation, fetch scheduling, AUDxLC/LEN reload, AUDxDAT
timing, AUDx INTREQ generation). So: scalar-correct Paula first, NEON
backend after. This issue covers the "scalar-correct first" half.

Starting state: `external/rigel/src/chipset/paula/audio.c` already
implemented the AUD0-3 channel state machine, but with zero visibility —
no trace points existed, and on the Bellatrix side nothing called
`bellatrix_machine_audio_left()/_right()` at all (no buffer, no host audio
driver, no resampler — `src/audio/mixer.c`/`mixer.h` were 0-byte
placeholders).

## What was implemented

**Timing instrumentation** — 6 cycle-stamped trace points, all 4 channels,
in `external/rigel/src/chipset/paula/audio.c`: AUDxPER write, AUDLC/AUDLEN
reload, DMA word fetch, period elapsed, AUDxDAT write, INTREQ AUDx raise.
Implemented via Rigel's existing structured event mechanism
(`rigel_log_event()`/`rigel_log_event_fn_t` in `rigel_config.h` +
`src/debug/log.c`) rather than Bellatrix's `core_log.h` — Paula audio runs
entirely inside Rigel on Core 2 (not a cross-core boundary; see
`[[issue_multicore_boundary_logging]]`), and Rigel's own architecture
("host owns I/O") rules out calling Bellatrix's `kprintf` macros directly
from a portable library. The mechanism was already wired host-side
(`machine_rigel.c:207`) and already gated by the same
`g_rtrace.enabled`/`BELLATRIX_RIGEL_TRACE_BUILD` flag as
`[RIGEL-IPL]`/`[RIGEL-DMACON]` — no new flag needed. Consumer
(`machine_rigel_log_event()` in `machine_rigel_trace.c`) prints
`[RIGEL-AUDIO-PER/TICK/IRQ/RELOAD/FETCH/DAT]`, each stamped with
`rigel_get_time(g_rigel)` read live at the moment the event fires.

**Bug found and fixed in the instrumentation itself**: the per-event-type
trace cap (`AUDIO_TRACE_LIMIT`, copied from the 512 used in
`copper_exec.c`/`slot_scheduler.c` for narrow late-triggered conditions)
was the wrong tool for an event meant to be observed continuously across a
boot — it's a single counter shared across all 4 channels, and with period
ticks firing every ~200-900 CCK per channel, 512 exhausts within the first
fraction of a second. This caused two real-boot captures (frame ~800,
cyc≈56-69M) to show zero `TICK`/`RELOAD`/`FETCH`/`IRQ`/`DAT` events at all,
which looked like a chipset bug at first. Traced the call chain
(`rigel_chipset_step()` in `chipset.c:74-76`, `rigel_dma_apply_setclr()` in
`dma_domain.c`) and confirmed the wiring was correct — `audio_step()` runs
unconditionally every chipset step with DMACON's audio bits intact. Raised
`AUDIO_TRACE_LIMIT` to `1000000u`.

**Cadence validated against a real boot** (after the cap fix): for all 4
channels, `AUDLEN` reloads to a small fixed value (ch0=1, ch1-3=2 words),
each word is consumed over exactly 2 period ticks (one word = 2 packed
8-bit samples, matching real Paula hardware), `RELOAD`→`FETCH`→`IRQ` land
on the same cycle whenever the reload immediately empties, and retrigger
spacing is exactly proportional to `audlen×audper` in every channel. This
is a continuous one-shot retrigger of a short sample/test-tone, not
starvation — internally consistent timing across all 4 channels, which is
this issue's core goal.

**Buffer consumer (push side)** — `src/audio/mixer.c`/`mixer.h` implemented
as a 2048-sample stereo ring buffer (drop-oldest-on-full, matching the
`RuntimeEventQueue` style in `runtime/event.c`). Added
`BellatrixMachine.audio_queue` and `bellatrix_machine_on_audio_sample_ready()`,
wired to `RIGEL_EVENT_HBLANK` (the ~15 kHz tick `rigel_audio.h` documents
as natural for `rigel_get_audio_sample()`) in **both** `rigel_step()` call
sites: `core_chipset.c` (multicore) and `machine_quantum_step()` in
`machine_rigel_step.c` (single-core/harness — this path had its own inline
`FRAME_READY` handling instead of calling `bellatrix_machine_on_frame_ready()`;
unified it, since that's also where the new per-frame `[RIGEL-AUDIO-QUEUE]`
diagnostic lives). `mixer.c` had to be added to both build systems
(`cmake/bellatrix-variant.cmake` and `tools/harness/CMakeLists.txt`) since
it was never compiled into either.

Verified end-to-end on a real harness run: `count=2048` (always full),
`dropped` growing by exactly 312/frame (= PAL scanlines = HBLANKs/frame) —
internally consistent, confirms the push path is alive. No pop consumer
exists yet (no host output driver — see below), so filling and discarding
the oldest sample every push is the correct, expected steady state.

**Host output driver research**: asked to decide HDMI vs I2S; chose HDMI,
then researched the actual requirement before implementing. Bare-metal
HDMI audio needs VCHIQ (the VideoCore's RPC channel, since the GPU
firmware does the PCM-into-HDMI mixing, not the ARM core) — no public
spec, known bare-metal implementations are 10,000+ lines ported from Linux
kernel internals, multi-year-old RPi forum threads from experienced
bare-metal developers never reached a complete solution. Checked upstream
Emu68 for existing audio/VCHIQ code — none exists. I2S is the documented,
register-level alternative. **Decision deferred** — doesn't block the
mixer/resampler work, which operates on the ring buffer regardless of
final output target. See `[[issue_paula_audio_simd_backend]]` for what's
still open.

## Validation done

- Rigel's own `test_audio` unit test exercises the new trace events, prints
  correctly via default stderr sink.
- Full Bellatrix build (`BELLATRIX_MULTICORE_BUILD=1 BELLATRIX_LOGS=1
  BELLATRIX_BTSTACK=1 BELLATRIX_USBSTACK=1`) — exit 0 at every stage of
  this work; `strings` on `Emu68.img` confirmed all 6 trace format strings
  present after each change.
- `tools/harness` build — exit 0, confirmed `mixer.c` links once added to
  its CMakeLists.
- QEMU boot smoke test — unaffected at every stage.
- Real Kickstart/AmigaOS boot captures (via the user's environment, not
  reproducible in the sandbox used for this session — see
  `[[issue_paula_audio_simd_backend]]` for that limitation) — used to
  validate cadence and the queue push/drop behavior.
- (Noted, not this work's fault: a pre-existing, unrelated build break in
  Rigel's own `test_copper.c` — wrong arg count for
  `rigel_copper_domain_step` — found incidentally, not touched.)

## Files touched

- `external/rigel/src/chipset/paula/audio.c` (6 trace points, all 4 channels)
- `external/rigel/include/rigel/rigel_config.h` (6 new `RIGEL_LOG_EVENT_AUDIO_*` IDs)
- `src/machine/machine_rigel_trace.c` (`machine_rigel_log_event()` cases)
- `src/audio/mixer.c` / `mixer.h` (ring buffer, was empty stub)
- `src/machine/machine.h` (`BellatrixMachine.audio_queue`,
  `bellatrix_machine_on_audio_sample_ready()` declaration)
- `src/machine/machine_rigel.c` (`audio_mixer_init()` in init/reset)
- `src/machine/machine_rigel_step.c` (HBLANK hook, FRAME_READY unification)
- `src/runtime/core_chipset.c` (HBLANK hook, multicore path)
- `cmake/bellatrix-variant.cmake`, `tools/harness/CMakeLists.txt`
  (registered `src/audio/mixer.c`)

## What's still open

See `AI_context/issue_paula_audio_simd_backend.md`: the `word=0000`
residual in fetch traces, external-reference timing validation, the NEON
mixer/resampler implementation itself, and the deferred I2S-vs-HDMI
decision.
