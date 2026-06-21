// src/audio/mixer.h
#ifndef BELLATRIX_AUDIO_MIXER_H
#define BELLATRIX_AUDIO_MIXER_H

#include <stdbool.h>
#include <stdint.h>

#define AUDIO_MIXER_QUEUE_SIZE 2048u

typedef struct AudioSample {
    int16_t left;
    int16_t right;
} AudioSample;

typedef struct AudioMixerQueue {
    AudioSample samples[AUDIO_MIXER_QUEUE_SIZE];

    uint32_t head;
    uint32_t tail;
    uint32_t count;

    uint64_t dropped_samples;
} AudioMixerQueue;

void audio_mixer_init(AudioMixerQueue *queue);

/* Pushes a sample; drops the oldest queued sample (and counts it) when full,
 * so the queue always reflects the most recent audio rather than stalling. */
void audio_mixer_push(AudioMixerQueue *queue, int16_t left, int16_t right);

bool audio_mixer_pop(AudioMixerQueue *queue, AudioSample *sample_out);

bool audio_mixer_is_empty(const AudioMixerQueue *queue);
uint32_t audio_mixer_count(const AudioMixerQueue *queue);
uint64_t audio_mixer_dropped(const AudioMixerQueue *queue);

#endif
