/* Real-thread SPSC stress for the posted-write queue, built with TSAN
 * (see CMakeLists). Proves the acquire/release pairs establish
 * happens-before for the payload: the consumer must observe every entry,
 * in order, with intact contents, across many ring wraps. */

#include "runtime/posted_writes.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>

#define STRESS_ENTRIES (POSTED_WRITES_CAPACITY * 400u)

static PostedWriteQueue s_queue;
static uint64_t s_consumed;
static int s_order_ok = 1;

static void stress_apply(void *ctx, const PostedWrite *w)
{
    uint32_t expect = (uint32_t)s_consumed;

    (void)ctx;
    if (w->value != expect * 7u)
        s_order_ok = 0;
    if (w->stamp_cck != (uint64_t)expect)
        s_order_ok = 0;
    if (w->addr != 0x00DFF000u + (expect & 0x1FEu))
        s_order_ok = 0;
    if (w->size != 2u)
        s_order_ok = 0;
    s_consumed++;
}

static void *producer_main(void *arg)
{
    uint32_t i;

    (void)arg;
    for (i = 0; i < STRESS_ENTRIES; i++) {
        PostedWrite w = {
            .stamp_cck = i,
            .addr = 0x00DFF000u + (i & 0x1FEu),
            .value = i * 7u,
            .size = 2u,
        };
        while (!posted_writes_try_push(&s_queue, &w))
            sched_yield();
    }
    return 0;
}

static void *consumer_main(void *arg)
{
    (void)arg;
    while (s_consumed < STRESS_ENTRIES) {
        if (posted_writes_apply(&s_queue, UINT64_MAX, stress_apply, 0) == 0u)
            sched_yield();
    }
    return 0;
}

int main(void)
{
    pthread_t producer;
    pthread_t consumer;

    posted_writes_reset(&s_queue);

    if (pthread_create(&producer, 0, producer_main, 0) != 0)
        return 1;
    if (pthread_create(&consumer, 0, consumer_main, 0) != 0)
        return 2;
    if (pthread_join(producer, 0) != 0)
        return 3;
    if (pthread_join(consumer, 0) != 0)
        return 4;

    if (s_consumed != STRESS_ENTRIES)
        return 5;
    if (!s_order_ok)
        return 6;
    if (posted_writes_depth(&s_queue) != 0u)
        return 7;
    return 0;
}
