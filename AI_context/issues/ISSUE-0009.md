---
id: ISSUE-0009
title: "Paula audio CPU↔chipset sync — harness choppiness"
status: done
priority: high
type: bug
owner: agent
created_at: 2026-06-26
updated_at: 2026-06-26
tags:
  - paula
  - audio
  - timing
  - harness
  - quantum
related_files:
  - tools/harness/main.c
  - tools/harness/musashi_backend.c
  - src/host/posix/pal_posix.c
  - src/machine/machine_rigel_step.c
---

# Issue: Paula audio sounds choppy in the harness — mostly fixed

## Status: CLOSED (2026-06-26)

Fix implementado: SDL audio prebuffered + CPU quantum fixo (454 ciclos) +
queue-throttled. Time drift ratio estável ~1.00, 50fps, áudio muito mais
suave. "Not claimed perfect" era cautela técnica — se houver regressão
audível em hardware real, abrir issue novo com dados concretos.

## Status: mostly fixed — root cause found in harness CPU quantum scheduling

Interactive testing with `KS13.rom` + `src/disks/battle.adf` confirmed that
Paula's internal AUDx timing was not the main cause of the choppy host audio.
The harness was feeding Musashi with `bellatrix_machine_recommended_cpu_quantum()`,
which included `rigel_get_next_bus_change()`. Once bitplanes/audio DMA became
active, bus/slot deadlines could occur every few CCK, so Musashi was called
hundreds of thousands of times per second with tiny quanta. The emulator then
fell behind wall-clock time, the SDL audio queue drained to one 1024-frame
block (~23 ms), and playback became choppy.

Fix now in place:

- SDL audio is prebuffered and queue-throttled in `src/host/posix/pal_posix.c`.
- Harness CPU quantum now defaults to the fixed caller cap (`QUANTUM = 454`
  M68K cycles) instead of being cut by every bus/slot deadline. Bus accesses
  still call `harness_sync_cpu_progress()` and `machine_flush_for_bus()`, so
  register/chip RAM accesses remain synchronized at the access point.
- The old deadline-limited CPU quantum can be restored with
  `HARNESS_CPU_DEADLINE_QUANTUM=1` for comparison.
- Diagnostics remain available: `HARNESS_AUDIO_QUEUE_TRACE=1`,
  `HARNESS_TIME_DRIFT_TRACE=1`, `HARNESS_PERF_TRACE=1`, and
  `HARNESS_CCK_GAP_TRACE=1`.

Observed result after the quantum change: the same Battle run holds
`[TIME-DRIFT] ratio` around `1.00` for more than 30 seconds and the harness
returns to about 50 fps. Audio is much smoother, though not claimed perfect.

Important diagnostic signature before the fix:

```text
[AUDIO-QUEUE] queued=23ms min=23ms max=23ms ...
[TIME-DRIFT] real=47.430s emulated=20.205s ratio=0.4260 ...
[HARNESS-PERF] steps=300k+ cck=~1.0M rigel_ms=~120-140 ...
```

The key signal was not `rigel_ms`; it was `steps/cck`: the harness was doing
many `rigel_step()` calls averaging only ~3-4 CCK each, with overhead outside
Rigel dominating. With the fixed CPU quantum, the time drift remains near 1.0.

## Why this exists as a separate issue

`AI_context/consolidated/issue_paula_audio_timing.md` marks Paula's
AUD0-3 timing as "resolved," but that resolution was narrower: it validated
that **AUDLEN/AUDPER/fetch/IRQ are internally self-consistent**, not whether
the **host-side audio actually sounds right**.

## The actual audio path that's choppy

A **pre-existing** audio pipeline in `tools/harness/main.c` (lines ~982-1069):

```c
uint32_t quantum = bellatrix_machine_recommended_cpu_quantum((uint32_t)QUANTUM);
int used = cpu_backend_run(musashi_backend_get(), quantum);
...
audio_acc += (uint64_t)(unsigned)used * AUDIO_RATE;
while (audio_acc >= M68K_HZ) {
    audio_acc -= M68K_HZ;
    pal_audio_push_sample(bellatrix_machine_audio_left(),
                          bellatrix_machine_audio_right());
}
```

`bellatrix_machine_audio_left/right()` just reads `rigel_get_audio_sample()` —
Paula's *current* mixed L/R value, a snapshot.

## Confirmed root cause: CPU quantum fragmented by bus/slot deadlines

`bellatrix_machine_recommended_cpu_quantum()` used both:

```c
rigel_get_next_deadline(g_rigel);
rigel_get_next_bus_change(g_rigel);
```

When DMA slots are active, `rigel_get_next_bus_change()` can be only a few CCK
away, so the host calls Musashi far too often. The fix is harness-specific:
default to the caller's fixed cap (`454` M68K cycles) and use bus-access
flushing for correct access-time state.

## Diagnostics implemented

1. **`HARNESS_AUDIO_DUP_TRACE=1`** — duplicate sample counter (86-94% is expected upsampling)
2. **`HARNESS_CCK_GAP_TRACE=1`** — general-purpose chipset-step gap tracker
3. **`HARNESS_TIME_DRIFT_TRACE=1`** — wall-clock vs. emulated-time ratio (target ~1.0)
4. **`HARNESS_AUDIO_QUEUE_TRACE=1`** — SDL queued-audio depth in milliseconds
5. **`HARNESS_PERF_TRACE=1`** — per-second Rigel step count, CCK advanced, timing

## Next step

The main choppiness source is fixed for the harness. Remaining audio
roughness should be investigated separately as Paula fidelity/output quality
(see ISSUE-0010 for NEON mixer). Useful follow-ups:

- Compare Battle with and without `HARNESS_CPU_DEADLINE_QUANTUM=1` only when
  deliberately reproducing the old behavior.
- Keep `HARNESS_AUDIO_QUEUE_TRACE=1 HARNESS_TIME_DRIFT_TRACE=1` for quick
  sanity checks; realtime should stay close to ratio 1.0.
- If audio is still imperfect while ratio is stable, inspect Paula sample
  interpolation/mixing and DMA fetch fidelity rather than harness pacing.
