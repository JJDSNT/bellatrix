// src/audio/mixer.c

#include "audio/mixer.h"

#include <string.h>

void audio_mixer_init(AudioMixerQueue *queue)
{
    if (!queue) {
        return;
    }

    memset(queue, 0, sizeof(*queue));
}

void audio_mixer_push(AudioMixerQueue *queue, int16_t left, int16_t right)
{
    if (!queue) {
        return;
    }

    if (queue->count >= AUDIO_MIXER_QUEUE_SIZE) {
        queue->head = (queue->head + 1u) % AUDIO_MIXER_QUEUE_SIZE;
        queue->count--;
        queue->dropped_samples++;
    }

    queue->samples[queue->tail].left  = left;
    queue->samples[queue->tail].right = right;
    queue->tail = (queue->tail + 1u) % AUDIO_MIXER_QUEUE_SIZE;
    queue->count++;
}

bool audio_mixer_pop(AudioMixerQueue *queue, AudioSample *sample_out)
{
    if (!queue || !sample_out || queue->count == 0u) {
        return false;
    }

    *sample_out = queue->samples[queue->head];
    queue->head = (queue->head + 1u) % AUDIO_MIXER_QUEUE_SIZE;
    queue->count--;
    return true;
}

bool audio_mixer_is_empty(const AudioMixerQueue *queue)
{
    return !queue || queue->count == 0u;
}

uint32_t audio_mixer_count(const AudioMixerQueue *queue)
{
    return queue ? queue->count : 0u;
}

uint64_t audio_mixer_dropped(const AudioMixerQueue *queue)
{
    return queue ? queue->dropped_samples : 0u;
}
