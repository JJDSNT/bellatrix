// AI_context/issue_paula_audio_cpu_chipset_sync.md

# Issue: Paula audio sounds choppy in the harness — CPU↔chipset stepping is too coarse/stale for sample extraction

## Status: open — duplicate-rate hypothesis (#2) mostly retracted (expected
upsampling, not a bug); real anomaly is a long stale-freeze pattern,
pointing at #1. General CCK-gap-vs-bus-touch instrumentation in progress
to confirm before fixing anything.

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

## Two likely root causes, found by reading the code (not yet confirmed by instrumentation)

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

## Proposed fix direction (not implemented, not yet agreed)

Rigel's own API includes `rigel_step_until()`
(`external/rigel/include/rigel/rigel_time.h`) specifically for stepping to
an exact target time, instead of stepping by an arbitrary `cycles` amount
and hoping it lines up. The likely fix: before each audio sample
extraction (both in `main.c`'s loop and in the ring-buffer HBLANK path),
call `rigel_step_until()` (or equivalent) to guarantee Paula's state is
caught up to the *exact* CCK corresponding to that sample's nominal time,
rather than reading whatever stale value happens to be sitting in
`audio_state_t` after the last unrelated bus touch.

## Suggested next step before fixing

Per this project's own working pattern this session: instrument before
fixing.

- **Done**: duplicate-sample counter added directly in `main.c`'s audio
  extraction loop, gated by `HARNESS_AUDIO_DUP_TRACE=1` (off by default,
  one int check when disabled). Counts total samples pushed to
  `pal_audio_push_sample()`, how many were exact `(left, right)` repeats
  of the immediately preceding one, and the longest consecutive run of
  repeats — printed once per second of audio as
  `[AUDIO-DUP] pushed=... duplicate=... (...%) max_run=...`. Run with
  `HARNESS_AUDIO_DUP_TRACE=1 ./run.sh harness`. A high duplicate
  percentage and/or a large `max_run` (hundreds+) would confirm root
  cause #2 directly. Not run yet — needs a real interactive session
  (SDL display), which isn't available in the sandbox this was written in.
- **Not done yet**: log the CCK delta between consecutive
  `harness_sync_cpu_progress()` calls (or correlate against the
  `[RIGEL-AUDIO-TICK]`/`[RIGEL-AUDIO-QUEUE]` cadence already in place)
  during a session with audible choppiness, to confirm root cause #1's
  bus-touch gaps correlate with the stutter independently of #2.

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

## Files to revisit

- `tools/harness/main.c` (lines ~982-1069 — the audio sample-rate-conversion loop)
- `tools/harness/musashi_backend.c` (`harness_sync_cpu_progress()`, its call sites in `harness_read`/`harness_write`)
- `src/host/posix/pal_posix.c` (`bellatrix_runtime_publish_cpu_cycles()`, `pal_audio_push_sample()` — two definitions in this file, check which is active and what backend (SDL?) it drives)
- `external/rigel/include/rigel/rigel_time.h` (`rigel_step_until()` — likely fix primitive)
- `src/machine/machine_rigel_step.c` (`machine_quantum_step()` — same stepping path on the bare-metal/single-core side)
- `AI_context/consolidated/issue_paula_audio_timing.md` (what "resolved" actually covers — internal cadence, not this)
- `AI_context/issue_paula_audio_neon_mixer.md` (the NEON mixer is downstream of whichever consumer reads this queue — building it before this is fixed means polishing the wrong samples)
