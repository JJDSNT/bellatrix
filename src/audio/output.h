// src/audio/output.h
#ifndef BELLATRIX_AUDIO_OUTPUT_H
#define BELLATRIX_AUDIO_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "audio/mixer.h"

#define BELLATRIX_AUDIO_OUTPUT_RATE_HZ 48000u
#define BELLATRIX_AUDIO_OUTPUT_QUEUE_SIZE 4096u

typedef struct BellatrixAudioOutputStats {
    uint64_t produced_samples;
    uint64_t consumed_samples;
    uint64_t dropped_samples;
    uint64_t underrun_samples;
} BellatrixAudioOutputStats;

void bellatrix_audio_output_init(void);
void bellatrix_audio_output_reset(void);

/* Advance the fixed-rate PCM producer by `cck_cycles` Rigel/Amiga clock
 * cycles. The CCK-to-output-rate conversion uses the current Rigel clock
 * (PAL/NTSC/custom), not a hard-coded video standard. This is intentionally
 * separate from the HBLANK diagnostic queue: physical output devices need a
 * steady stream at the sink rate. */
void bellatrix_audio_output_tick(uint32_t cck_cycles);

bool bellatrix_audio_output_pop(AudioSample *sample_out);
uint32_t bellatrix_audio_output_count(void);
BellatrixAudioOutputStats bellatrix_audio_output_stats(void);

#endif
