// src/audio/output.c

#include "audio/output.h"

#include "machine/machine.h"

#include "rigel/rigel_time.h"

#include <string.h>

enum {
    BELLATRIX_AUDIO_DEFAULT_CLOCK_HZ = 7093790u,
};

typedef struct BellatrixAudioOutput {
    AudioSample samples[BELLATRIX_AUDIO_OUTPUT_QUEUE_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint64_t sample_acc;
    BellatrixAudioOutputStats stats;
} BellatrixAudioOutput;

static BellatrixAudioOutput s_audio_output;

static void bellatrix_audio_output_push(int16_t left, int16_t right)
{
    if (s_audio_output.count >= BELLATRIX_AUDIO_OUTPUT_QUEUE_SIZE) {
        s_audio_output.head =
            (s_audio_output.head + 1u) % BELLATRIX_AUDIO_OUTPUT_QUEUE_SIZE;
        s_audio_output.count--;
        s_audio_output.stats.dropped_samples++;
    }

    s_audio_output.samples[s_audio_output.tail].left = left;
    s_audio_output.samples[s_audio_output.tail].right = right;
    s_audio_output.tail =
        (s_audio_output.tail + 1u) % BELLATRIX_AUDIO_OUTPUT_QUEUE_SIZE;
    s_audio_output.count++;
    s_audio_output.stats.produced_samples++;
}

void bellatrix_audio_output_init(void)
{
    bellatrix_audio_output_reset();
}

void bellatrix_audio_output_reset(void)
{
    memset(&s_audio_output, 0, sizeof(s_audio_output));
}

void bellatrix_audio_output_tick(uint32_t cck_cycles)
{
    struct RigelContext *rigel = bellatrix_machine_rigel_ctx();
    uint32_t clock_hz = rigel ? (uint32_t)rigel_get_clock_hz(rigel) : 0u;

    if (clock_hz == 0u)
        clock_hz = BELLATRIX_AUDIO_DEFAULT_CLOCK_HZ;

    s_audio_output.sample_acc +=
        (uint64_t)cck_cycles * (uint64_t)BELLATRIX_AUDIO_OUTPUT_RATE_HZ;

    while (s_audio_output.sample_acc >= clock_hz) {
        s_audio_output.sample_acc -= clock_hz;
        bellatrix_audio_output_push(bellatrix_machine_audio_left(),
                                    bellatrix_machine_audio_right());
    }
}

bool bellatrix_audio_output_pop(AudioSample *sample_out)
{
    if (!sample_out || s_audio_output.count == 0u) {
        s_audio_output.stats.underrun_samples++;
        return false;
    }

    *sample_out = s_audio_output.samples[s_audio_output.head];
    s_audio_output.head =
        (s_audio_output.head + 1u) % BELLATRIX_AUDIO_OUTPUT_QUEUE_SIZE;
    s_audio_output.count--;
    s_audio_output.stats.consumed_samples++;
    return true;
}

uint32_t bellatrix_audio_output_count(void)
{
    return s_audio_output.count;
}

BellatrixAudioOutputStats bellatrix_audio_output_stats(void)
{
    return s_audio_output.stats;
}
