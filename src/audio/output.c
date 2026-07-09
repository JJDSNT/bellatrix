// src/audio/output.c

#include "audio/output.h"

#include "machine/machine.h"

#include "rigel/rigel_time.h"

#include <string.h>

enum {
    BELLATRIX_AUDIO_DEFAULT_CLOCK_HZ = 7093790u,

    /* --- Sync layer: dynamic rate control (DRC) ---------------------------
     * The producer resamples Paula → 48 kHz in EMULATED time; the HDMI sink
     * drains the queue in REAL time. Those are two nominally-48 kHz clocks that
     * inevitably drift (the sink's real rate is tied to the pixel clock, ours to
     * the emulated CCK), and sample production is jittery per quantum. Left
     * alone the queue slowly walks to empty (underrun/click) or full (drop),
     * even when the machine keeps up on average.
     *
     * DRC closes the loop: keep the queue near TARGET_DEPTH by nudging the
     * effective sink rate a few tenths of a percent (below target → produce a
     * touch faster, above → a touch slower). The ±0.4 % clamp keeps the loop
     * stable and the pitch shift inaudible. This makes audio clean whenever
     * production ~matches consumption; it does NOT manufacture throughput the
     * machine can't sustain (sub-real-time emulation still underruns — that is a
     * speed axis, not a sync one). See [[bellatrix-audio-realtime-ratio]]. */
    BELLATRIX_AUDIO_TARGET_DEPTH = 1536u,   /* ~32 ms latency at 48 kHz */
    BELLATRIX_AUDIO_DRC_MAX_ADJ  = 192u,    /* ±0.4 % of 48 kHz */
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

/* DRC: effective sink rate = 48 kHz nudged toward keeping the queue at
 * TARGET_DEPTH. Proportional loop, clamped to ±DRC_MAX_ADJ (±0.4 %). */
static uint32_t bellatrix_audio_output_rate(void)
{
    int32_t delta = (int32_t)BELLATRIX_AUDIO_TARGET_DEPTH
                    - (int32_t)s_audio_output.count;
    int32_t adj = ((int32_t)BELLATRIX_AUDIO_DRC_MAX_ADJ * delta)
                  / (int32_t)BELLATRIX_AUDIO_TARGET_DEPTH;

    if (adj > (int32_t)BELLATRIX_AUDIO_DRC_MAX_ADJ)
        adj = (int32_t)BELLATRIX_AUDIO_DRC_MAX_ADJ;
    else if (adj < -(int32_t)BELLATRIX_AUDIO_DRC_MAX_ADJ)
        adj = -(int32_t)BELLATRIX_AUDIO_DRC_MAX_ADJ;

    return (uint32_t)((int32_t)BELLATRIX_AUDIO_OUTPUT_RATE_HZ + adj);
}

void bellatrix_audio_output_tick(uint32_t cck_cycles)
{
    struct RigelContext *rigel = bellatrix_machine_rigel_ctx();
    uint32_t clock_hz = rigel ? (uint32_t)rigel_get_clock_hz(rigel) : 0u;
    uint32_t out_rate = bellatrix_audio_output_rate();

    if (clock_hz == 0u)
        clock_hz = BELLATRIX_AUDIO_DEFAULT_CLOCK_HZ;

    s_audio_output.sample_acc += (uint64_t)cck_cycles * (uint64_t)out_rate;

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
