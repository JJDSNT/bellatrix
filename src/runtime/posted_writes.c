#include "runtime/posted_writes.h"

#include <stddef.h>

void posted_writes_reset(PostedWriteQueue *q)
{
    if (q == NULL)
        return;
    atomic_store_explicit(&q->head, 0u, memory_order_release);
    atomic_store_explicit(&q->tail, 0u, memory_order_release);
}

uint32_t posted_writes_depth(const PostedWriteQueue *q)
{
    uint32_t head;
    uint32_t tail;

    if (q == NULL)
        return 0u;
    head = atomic_load_explicit(&q->head, memory_order_acquire);
    tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    return head - tail;
}

bool posted_writes_try_push(PostedWriteQueue *q, const PostedWrite *write)
{
    uint32_t head;
    uint32_t tail;

    if (q == NULL || write == NULL)
        return false;

    head = atomic_load_explicit(&q->head, memory_order_relaxed);
    tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head - tail >= POSTED_WRITES_CAPACITY)
        return false;

    q->ring[head & (POSTED_WRITES_CAPACITY - 1u)] = *write;
    atomic_store_explicit(&q->head, head + 1u, memory_order_release);
    return true;
}

uint64_t posted_writes_next_stamp(const PostedWriteQueue *q)
{
    uint32_t head;
    uint32_t tail;

    if (q == NULL)
        return UINT64_MAX;

    tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail == head)
        return UINT64_MAX;
    return q->ring[tail & (POSTED_WRITES_CAPACITY - 1u)].stamp_cck;
}

uint32_t posted_writes_apply(PostedWriteQueue *q, uint64_t limit,
                             PostedWriteApplyFn apply, void *ctx)
{
    uint32_t tail;
    uint32_t head;
    uint32_t applied = 0u;

    if (q == NULL || apply == NULL)
        return 0u;

    tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    head = atomic_load_explicit(&q->head, memory_order_acquire);

    while (tail != head) {
        const PostedWrite *w = &q->ring[tail & (POSTED_WRITES_CAPACITY - 1u)];

        if (w->stamp_cck > limit)
            break;
        apply(ctx, w);
        tail++;
        applied++;
    }
    atomic_store_explicit(&q->tail, tail, memory_order_release);
    return applied;
}
