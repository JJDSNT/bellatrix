/* Real-thread SPSC stress for the audio output queue, built with TSAN.
 * The producer (chipset owner in the real system) drives
 * bellatrix_audio_output_tick(); the consumer (Core 0 HDMI poll) pops.
 * Samples carry a wrapping sequence in the left channel so the consumer
 * can verify nothing is lost, duplicated or reordered across ring wraps. */

#include "audio/output.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>

/* ---- stubs for output.c's machine/rigel hooks (producer thread only) ---- */
struct RigelContext;
struct RigelContext *bellatrix_machine_rigel_ctx(void) { return 0; }

typedef uint32_t rigel_u32;
typedef uint64_t rigel_cycle_t;
rigel_u32 rigel_get_clock_hz(const struct RigelContext *ctx)
{
    (void)ctx;
    return 0;
}

static int16_t s_next_sample;
int16_t bellatrix_machine_audio_left(void)
{
    return ++s_next_sample;
}
/* Argument evaluation order of push(left(), right()) is unspecified in C,
 * so the right channel must not depend on the left stub's side effect. */
int16_t bellatrix_machine_audio_right(void)
{
    return 0;
}

/* ------------------------------------------------------------------------ */

#define STRESS_SAMPLES 400000u

static _Atomic uint64_t s_popped;

static void *producer_main(void *arg)
{
    (void)arg;
    while (atomic_load_explicit(&s_popped, memory_order_relaxed) <
           STRESS_SAMPLES) {
        /* ~13 samples per call at the default PAL clock and 48 kHz. Keep
         * headroom so drop-on-full never fires: this test asserts lossless
         * transport, not overflow policy. */
        if (bellatrix_audio_output_count() <
            BELLATRIX_AUDIO_OUTPUT_QUEUE_SIZE - 64u)
            bellatrix_audio_output_tick(1000u);
        else
            sched_yield();
    }
    return 0;
}

static int s_sequence_ok = 1;

static void *consumer_main(void *arg)
{
    AudioSample sample;
    int16_t expected = 0;
    int primed = 0;

    (void)arg;
    while (atomic_load_explicit(&s_popped, memory_order_relaxed) <
           STRESS_SAMPLES) {
        if (!bellatrix_audio_output_pop(&sample)) {
            sched_yield();
            continue;
        }
        if (!primed) {
            /* First observed sample anchors the sequence. */
            expected = sample.left;
            primed = 1;
        }
        if (sample.left != expected)
            s_sequence_ok = 0;
        expected = (int16_t)(expected + 1);
        atomic_fetch_add_explicit(&s_popped, 1u, memory_order_relaxed);
    }
    return 0;
}

int main(void)
{
    pthread_t producer;
    pthread_t consumer;
    BellatrixAudioOutputStats stats;

    bellatrix_audio_output_init();
    bellatrix_audio_output_set_drc(0);

    if (pthread_create(&producer, 0, producer_main, 0) != 0)
        return 1;
    if (pthread_create(&consumer, 0, consumer_main, 0) != 0)
        return 2;
    if (pthread_join(producer, 0) != 0)
        return 3;
    if (pthread_join(consumer, 0) != 0)
        return 4;

    if (!s_sequence_ok)
        return 5;

    stats = bellatrix_audio_output_stats();
    /* Nothing may be dropped in this run: the producer stops once the
     * consumer target is reached and the queue is deeper than any burst. */
    if (stats.dropped_samples != 0u)
        return 6;
    if (stats.consumed_samples < STRESS_SAMPLES)
        return 7;
    return 0;
}
