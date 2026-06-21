// AI_context/issue_paula_audio_cpu_chipset_sync.md

# Issue: Paula audio sounds choppy in the harness — cause not yet found

## Status: open — both original root-cause hypotheses (#1 and #2) have been
walked back after closer reading. Two general-purpose diagnostics are now
in place (CCK-gap tracer, wall-clock-drift tracer); the real cause is not
yet known and needs an interactive (SDL display) session to find, which
isn't available in this session's sandbox.

## Why this exists as a separate issue

`AI_context/consolidated/issue_paula_audio_timing.md` marks Paula's
AUD0-3 timing as "resolved," but that resolution was narrower than it
sounds: it validated that **AUDLEN/AUDPER/fetch/IRQ are internally
self-consistent** (the retrigger cadence matches `audlen × audper` in
every channel) using the new Rigel trace events. It never checked whether
the **host-side audio actually sounds right**, and it doesn't — the user
reports the harness's audio output is audibly choppy ("engasgado"). This
issue tracks that real, currently-unfixed problem. The consolidated doc's
"resolved" status should be read as "the instrumentation work and the
internal-cadence validation it enabled are done," not "Paula audio is
correct end to end."

## The actual audio path that's choppy (not the one Part 1 built)

This is a **third, pre-existing** audio pipeline, separate from both
Rigel's trace events and the `src/audio/mixer.c` ring buffer built in
`[[issue_paula_audio_timing]]`. It already plays real sound on the host via
SDL (`pal_audio_push_sample()` in `src/host/posix/pal_posix.c`), driven
from `tools/harness/main.c`'s main loop (lines ~982-1069):

```c
uint32_t quantum = bellatrix_machine_recommended_cpu_quantum((uint32_t)QUANTUM);
int used = cpu_backend_run(musashi_backend_get(), quantum);
...
audio_acc += (uint64_t)(unsigned)used * AUDIO_RATE;      // AUDIO_RATE = 44100
while (audio_acc >= M68K_HZ) {                           // M68K_HZ = 7093790
    audio_acc -= M68K_HZ;
    pal_audio_push_sample(bellatrix_machine_audio_left(),
                          bellatrix_machine_audio_right());
}
```

`bellatrix_machine_audio_left/right()` just reads
`rigel_get_audio_sample()` — Paula's *current* mixed L/R value, a snapshot,
not something that advances on its own.

## Two likely root causes, found by reading the code (both later walked back — see "Corrections" below; kept for the record of how this was investigated)

1. **Chipset time only advances when the CPU touches the bus.**
   `harness_sync_cpu_progress()` (`tools/harness/musashi_backend.c:80`,
   called from inside `harness_read`/`harness_write` at lines 1705, 1719,
   1728, 1743, 1760, 1801 — i.e. only from chip RAM / custom-register /
   slow-RAM bus accesses) is what calls `bellatrix_bridge_cpu_progress()`
   → `bellatrix_runtime_publish_cpu_cycles()` → (in the harness's POSIX
   PAL, `src/host/posix/pal_posix.c:131-135`) `bellatrix_machine_advance()`
   → `machine_quantum_step()` → `rigel_step()`. **If the M68K executes a
   stretch of bus-access-free instructions** (ALU-only loop, register-only
   work), Paula's internal state — including `audio_step()`'s period
   countdown and the mixed sample — does not advance at all during that
   stretch, no matter how many CCK "should" have elapsed. The mixed sample
   read by `bellatrix_machine_audio_left/right()` is stale until the next
   bus touch.
2. **The sample-rate-conversion loop in `main.c` can push duplicate
   samples.** The `while (audio_acc >= M68K_HZ)` loop reads
   `bellatrix_machine_audio_left/right()` fresh on every iteration, but
   nothing advances Paula's state *between* those reads — they all see the
   same snapshot. If one `cpu_backend_run()` quantum produces enough `used`
   cycles to cross the 44.1 kHz threshold more than once, the loop pushes
   N copies of the same sample instead of N samples representing how Paula
   actually evolved across that quantum. Audibly, repeated/duplicated
   samples read as exactly this kind of stutter/aliasing.

Both root causes trace back to the same thing: **sample extraction is not
guaranteed to happen at a point where Paula has actually been stepped to
the corresponding moment in chip time.** This is exactly the
"Paula↔beam/CCK↔CPU sync" failure mode the original audio issue's
governing rule warned about — Part 1's instrumentation validated Paula's
*internal* cadence but had no way to catch this, since it's a host
extraction problem, not a chipset state-machine problem.

## Why this likely also affects the new HBLANK/ring-buffer path

`src/audio/mixer.c`'s ring buffer (built in
`[[issue_paula_audio_timing]]`) is filled via the same underlying
`rigel_step()` call sites, gated on `RIGEL_EVENT_HBLANK` in the returned
event mask. If chipset stepping is similarly coarse/bursty there (worth
checking whether the bare-metal/multicore path has the same
bus-touch-gating as the harness, or steps on a fixed quantum regardless —
not yet confirmed), a single large step batch that crosses *multiple*
HBLANK boundaries would only report the event mask once, **dropping**
samples rather than duplicating them — a different symptom, same root
cause. Not confirmed either way yet; flagging so whoever picks this up
checks both consumers, not just the harness one.

## Proposed fix direction — superseded, kept for the record

The original plan was to call `rigel_step_until()`
(`external/rigel/include/rigel/rigel_time.h`) before each audio sample
extraction, to force Paula's state to the exact target CCK instead of
reading a possibly-stale snapshot. That was based on root cause #1's
"long freeze" framing, which the quantum-capping discovery above ruled
out — there's no staleness of the size that would require this. Not
pursuing this fix direction unless the interactive drift/gap data points
back at chipset staleness after all.

If the wall-clock-drift run instead confirms a vsync/PAL-rate mismatch,
the actual fix would be in `tools/harness/main.c`'s main loop: replace the
vsync-only pacing with an explicit real-time throttle (e.g. sleep to a
target wall-clock time per Amiga frame, independent of host display
refresh rate), or decouple audio delivery timing from frame-presentation
timing entirely. Not designed in detail yet — depends on what the
interactive run actually shows.

### Correction: the 86-94% duplicate rate is largely expected upsampling, not proof of a bug

Real session output (`HARNESS_AUDIO_DUP_TRACE=1`):

```
[AUDIO-DUP] pushed=661500  duplicate=661499  (100.0%) max_run=661499
[AUDIO-DUP] pushed=705600  duplicate=705599  (100.0%) max_run=705599
[AUDIO-DUP] pushed=749700  duplicate=745543  (99.4%)  max_run=726744
...
[AUDIO-DUP] pushed=1543500 duplicate=1332039 (86.3%)  max_run=726744
```

First pass at this data jumped to "root cause #2 confirmed" by comparing
86-94% against intuition rather than against Paula's actual native rate —
that was wrong. Redone properly: one host audio sample = `M68K_HZ / 2 /
AUDIO_RATE` ≈ 80.5 CCK. The channels seen in `[[issue_paula_audio_timing]]`'s
captures have `audper=214` (≈16.6 kHz native) and `audper=856`
(≈4.1 kHz native, three of the four channels). Nearest-neighbor upsampling
an ~4.1 kHz source to 44.1 kHz produces **~90% duplicates by construction**
— almost exactly the observed range. Also checked
`rigel_get_next_deadline()` (`external/rigel/src/core/rigel.c:277`): it
already includes `audio_cycles_to_next_event()` in its deadline
computation, so Rigel's quantum scheduler isn't ignoring audio's timing
need either — there's no scheduling gap there to blame.

So the steady-state 86-94% is mostly **correct, expected behavior** for
upsampling a sub-44.1kHz source, not a confirmed bug. What's NOT explained
by any legitimate audper value is the `max_run=726744` plateau — **16.5
real seconds** frozen on one exact sample. No realistic `audper` holds a
value that long; this is the actual anomaly worth chasing, and it points
at root cause #1 (bus-touch-gated stepping causing occasional long
freezes), not #2. Whether that freeze was just an idle/silent boot screen
(harmless) or a genuine multi-second stall during active content (the
actual "engasgado" the user hears) isn't known yet from this data alone —
need the general CCK-gap instrumentation below to tell the difference.

### Second correction: root cause #1 (long bus-touch-free freezes) doesn't hold either

Read `musashi_run()` (the `CpuBackend.run` implementation,
`tools/harness/musashi_backend.c:1951-1964`) closely:

```c
static int musashi_run(void *ctx, uint32_t cycles)
{
    s_run_sync_active = 1;
    s_run_sync_published = 0;
    used = m68k_execute((int)cycles);
    harness_sync_cpu_progress();   /* unconditional final flush */
    s_run_sync_active = 0;
    return used;
}
```

Two things this reveals:

1. The main loop calls this with `quantum = bellatrix_machine_recommended_cpu_quantum(QUANTUM)`,
   where `QUANTUM = 454` M68K cycles — a small, fixed cap. Musashi never
   executes more than ~454 cycles before control returns to the main loop.
2. **Regardless of how many bus touches happened during that quantum**, the
   function unconditionally calls `harness_sync_cpu_progress()` one more
   time right after `m68k_execute()` returns, before clearing
   `s_run_sync_active`. Since `s_run_sync_published` was reset to 0 at the
   start of this call, that final flush always publishes the *entire*
   `used` delta for the quantum, bus touches or not.

Together: the maximum possible staleness in Paula's state, even in the
total-silence worst case (zero bus touches all quantum), is bounded by
~454 M68K cycles (≈64 µs, ≈227 CCK) — nowhere near enough to produce a
multi-second freeze. **Root cause #1 as originally framed doesn't apply to
this loop.** The `max_run=726744` (~16.5s) plateau in the duplicate-trace
data is almost certainly just a genuinely silent boot/loading screen, not
a sync bug.

### New lead: no real-time throttle, only vsync — possible speed drift

`tools/harness/main.c`'s loop has no `usleep`/`SDL_Delay`/explicit
real-time pacing at all (confirmed by grep — only `PAL_Time_ReadCounter()`
calls, used for FPS display, not throttling). The only thing that blocks
the loop at all is `SDL_RenderPresent()`
(`src/host/posix/pal_posix.c:526`), called with `SDL_RENDERER_PRESENTVSYNC`
— i.e. the loop is paced by the **host display's vsync** (commonly 60 Hz),
once per *presented* Amiga frame, not by wall-clock time directly. Amiga
PAL software expects ~50 Hz. If the loop runs as fast as the host allows
within each Amiga frame's worth of iterations (it does — there's no
per-iteration throttle, only the one vsync wait after a frame completes),
and vsync paces frame delivery to 60 Hz instead of 50 Hz, emulated time
could run systematically ~20% fast — or, if the host can't keep up with
real-time rendering/audio overhead, systematically slow. Either direction
would desync the SDL audio queue (`SDL_QueueAudio`, real-time-rate
playback device) from production rate, independent of whether Paula's own
state machine is correct — a plausible source of audible choppiness that
has nothing to do with chipset timing at all.

### Diagnostics implemented so far

1. **`HARNESS_AUDIO_DUP_TRACE=1`** (`tools/harness/main.c`) — duplicate
   `(left, right)` sample counter. Implemented and run once (see
   "Correction" above); conclusion was that the steady-state rate is
   expected upsampling, not a smoking gun on its own.
2. **`HARNESS_CCK_GAP_TRACE=1`** (`tools/harness/musashi_backend.c`,
   `harness_cck_gap_trace()`) — general-purpose (not audio-specific) gap
   tracker: histograms the M68K-cycle delta `harness_sync_cpu_progress()`
   publishes each call, flags new max-gap records above 500 cycles with
   the PC at that point, prints a bucketed summary every 200,000 calls.
   Meant for diagnosing *any* subsystem that depends on regular chipset
   stepping (disk, serial, CIA timers — not just audio), per the request
   that motivated building it as general-purpose rather than audio-only.
   Tested in `--headless --cycles 5000000`: **zero output**, including zero
   max-gap records above 500 cycles. Consistent with the quantum-capping
   finding above — gaps physically cannot exceed ~454 cycles in this loop,
   so this tool currently has nothing to report for the harness's
   interactive path. Still useful for the bare-metal/multicore path or any
   future code path that doesn't have the same small-quantum-with-flush
   guarantee — kept in the tree for that.
3. **`HARNESS_TIME_DRIFT_TRACE=1`** (`tools/harness/main.c`) — wall-clock
   (`PAL_Time_ReadCounter`) vs. emulated-time (`total_cycles / M68K_HZ`)
   ratio, printed once per real second. Sanity-checked in `--headless`
   mode (no vsync at all): ratio settled around 2.7-4.7x, exactly the
   "runs as fast as the host allows" behavior expected for that mode —
   confirms the measurement itself is correct. **Not yet run
   interactively** (needs SDL display, not available in this sandbox) —
   that's the run that would actually confirm or rule out the vsync/PAL
   mismatch hypothesis. A ratio far from 1.0 in an interactive run would
   point straight at this; a ratio near 1.0 would mean the slowdown is
   somewhere else entirely (SDL buffer sizing, OS audio scheduling, etc.).

## Next step

Run an **interactive** session (real SDL display, not `--headless`) with
both `HARNESS_TIME_DRIFT_TRACE=1` and `HARNESS_AUDIO_DUP_TRACE=1` (and
optionally `HARNESS_CCK_GAP_TRACE=1`, though it's expected to stay quiet
per the analysis above) while the choppy audio is audible, and read
`[TIME-DRIFT]`'s ratio. That's the next real signal — not available in
this session's sandbox.

## Files to revisit

- `tools/harness/main.c` (lines ~982-1069 audio loop; the new
  `HARNESS_TIME_DRIFT_TRACE`/`HARNESS_AUDIO_DUP_TRACE` blocks)
- `tools/harness/musashi_backend.c` (`harness_sync_cpu_progress()`,
  `musashi_run()`'s unconditional final flush; the new
  `harness_cck_gap_trace()`)
- `src/host/posix/pal_posix.c` (`pal_audio_push_sample()` — `SDL_QueueAudio`
  based, line ~365; `SDL_RenderPresent`/vsync at line ~526; two
  `pal_audio_push_sample` definitions in this file — check which is active)
- `src/machine/machine_rigel_step.c` (`machine_quantum_step()` — same
  stepping path on the bare-metal/single-core side, worth checking whether
  it has the same small-quantum-with-unconditional-flush guarantee)
- `AI_context/consolidated/issue_paula_audio_timing.md` (what "resolved" actually covers — internal cadence, not this)
- `AI_context/issue_paula_audio_neon_mixer.md` (the NEON mixer is downstream of whichever consumer reads this queue — building it before this is fixed means polishing the wrong samples)
