// src/audio/output.h
#ifndef BELLATRIX_AUDIO_OUTPUT_H
#define BELLATRIX_AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "audio/mixer.h"

#define BELLATRIX_AUDIO_OUTPUT_RATE_HZ 48000u
/* Queue capacity (~341 ms at 48 kHz). The old 4096 (~85 ms) was the short link
 * vs the harness's ~683 ms SDL ring; a deeper queue lets the consumer prime a
 * real cushion and ride production jitter. The runtime latency knob is the prime
 * depth (bellatrix_audio_output_set_prime_frames), not this ceiling. */
#define BELLATRIX_AUDIO_OUTPUT_QUEUE_SIZE 16384u

typedef struct BellatrixAudioOutputStats {
    uint64_t produced_samples;
    uint64_t consumed_samples;
    uint64_t dropped_samples;
    uint64_t underrun_samples;
} BellatrixAudioOutputStats;

void bellatrix_audio_output_init(void);
void bellatrix_audio_output_reset(void);
void bellatrix_audio_output_set_enabled(int enabled);

/* Advance the fixed-rate PCM producer by `cck_cycles` Rigel/Amiga clock
 * cycles. The CCK-to-output-rate conversion uses the current Rigel clock
 * (PAL/NTSC/custom), not a hard-coded video standard. This is intentionally
 * separate from the HBLANK diagnostic queue: physical output devices need a
 * steady stream at the sink rate. */
void bellatrix_audio_output_tick(uint32_t cck_cycles);

bool bellatrix_audio_output_pop(AudioSample *sample_out);
uint32_t bellatrix_audio_output_count(void);
BellatrixAudioOutputStats bellatrix_audio_output_stats(void);

/* Enable/disable dynamic rate control (default enabled). Used by the harness
 * A/B to isolate whether the DRC or the queue degrades quality. */
void bellatrix_audio_output_set_drc(int enabled);

/* Priming depth: the consumer withholds output (feeding silence) until the queue
 * first fills to this many stereo frames, then plays; if the queue drains empty
 * it re-primes. This trades startup/refill latency for jitter immunity — the
 * same idea as the harness SDL ring's cushion. 0 disables priming (the default —
 * priming only helps near real-time; under sub-real-time emulation the queue
 * never reaches a threshold, so a non-zero prime would stay silent). Runtime-
 * settable so a later auto-adjust loop can tune latency vs. underrun. Clamped to
 * the queue capacity. */
void     bellatrix_audio_output_set_prime_frames(uint32_t frames);
uint32_t bellatrix_audio_output_prime_frames(void);
int      bellatrix_audio_output_is_primed(void);

#endif
