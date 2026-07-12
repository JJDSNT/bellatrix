#include "runtime/posted_writes.h"

#include <stdint.h>

/* Consumer harness: records applied entries in order. */
static PostedWrite s_applied[POSTED_WRITES_CAPACITY * 4u];
static uint32_t s_applied_count;

static void record_apply(void *ctx, const PostedWrite *w)
{
    (void)ctx;
    s_applied[s_applied_count++] = *w;
}

static PostedWrite make(uint64_t stamp, uint32_t addr, uint32_t value)
{
    return (PostedWrite){
        .stamp_cck = stamp, .addr = addr, .value = value, .size = 2u,
    };
}

int main(void)
{
    PostedWriteQueue q;
    PostedWrite w;
    uint32_t i;

    posted_writes_reset(&q);

    /* Empty queue: no stamp, nothing applied, zero depth. */
    if (posted_writes_next_stamp(&q) != UINT64_MAX) return __LINE__;
    if (posted_writes_apply(&q, UINT64_MAX, record_apply, 0) != 0u) return __LINE__;
    if (posted_writes_depth(&q) != 0u) return __LINE__;

    /* FIFO order and stamp-limited apply: entries after the limit stay. */
    for (i = 0; i < 8u; i++) {
        w = make(100u + i, 0xDFF180u + i * 2u, i);
        if (!posted_writes_try_push(&q, &w)) return __LINE__;
    }
    if (posted_writes_depth(&q) != 8u) return __LINE__;
    if (posted_writes_next_stamp(&q) != 100u) return __LINE__;

    s_applied_count = 0;
    if (posted_writes_apply(&q, 103u, record_apply, 0) != 4u) return __LINE__;
    if (s_applied_count != 4u) return __LINE__;
    for (i = 0; i < 4u; i++) {
        if (s_applied[i].stamp_cck != 100u + i) return __LINE__;
        if (s_applied[i].value != i) return __LINE__;
    }
    if (posted_writes_next_stamp(&q) != 104u) return __LINE__;

    /* Drain everything: remaining entries in order, queue empty after. */
    s_applied_count = 0;
    if (posted_writes_apply(&q, UINT64_MAX, record_apply, 0) != 4u) return __LINE__;
    if (s_applied[0].stamp_cck != 104u || s_applied[3].stamp_cck != 107u)
        return __LINE__;
    if (posted_writes_depth(&q) != 0u) return __LINE__;

    /* Full queue: try_push refuses without blocking (the liveness contract:
     * the producer can always fall back to the synchronous path — a pause
     * coinciding with a full queue cannot strand it inside the queue). */
    for (i = 0; i < POSTED_WRITES_CAPACITY; i++) {
        w = make(1000u + i, 0xDFF100u, i);
        if (!posted_writes_try_push(&q, &w)) return __LINE__;
    }
    w = make(9999u, 0xDFF100u, 0xDEADu);
    if (posted_writes_try_push(&q, &w)) return __LINE__;
    if (posted_writes_depth(&q) != POSTED_WRITES_CAPACITY) return __LINE__;

    /* One consumer slot freed unblocks exactly one producer slot. */
    s_applied_count = 0;
    if (posted_writes_apply(&q, 1000u, record_apply, 0) != 1u) return __LINE__;
    if (!posted_writes_try_push(&q, &w)) return __LINE__;
    if (posted_writes_try_push(&q, &w)) return __LINE__;

    /* Wrap-around: cursors are free-running; push/apply several capacities
     * worth of entries and verify content survives the index masking. */
    posted_writes_reset(&q);
    for (i = 0; i < POSTED_WRITES_CAPACITY * 3u; i++) {
        w = make(i, 0xDFF000u + (i & 0xFFu), i * 7u);
        if (!posted_writes_try_push(&q, &w)) return __LINE__;
        if (i % 2u == 1u) {
            s_applied_count = 0;
            if (posted_writes_apply(&q, i, record_apply, 0) != 2u)
                return __LINE__;
            if (s_applied[0].value != (i - 1u) * 7u) return __LINE__;
            if (s_applied[1].value != i * 7u) return __LINE__;
        }
    }
    if (posted_writes_depth(&q) != 0u) return __LINE__;

    /* Reset discards pending entries and restarts cursors. */
    w = make(5u, 0xDFF100u, 1u);
    (void)posted_writes_try_push(&q, &w);
    posted_writes_reset(&q);
    if (posted_writes_depth(&q) != 0u) return __LINE__;
    if (posted_writes_next_stamp(&q) != UINT64_MAX) return __LINE__;

    /* NULL robustness. */
    if (posted_writes_try_push(0, &w)) return __LINE__;
    if (posted_writes_try_push(&q, 0)) return __LINE__;
    if (posted_writes_apply(&q, UINT64_MAX, 0, 0) != 0u) return __LINE__;
    if (posted_writes_next_stamp(0) != UINT64_MAX) return __LINE__;
    if (posted_writes_depth(0) != 0u) return __LINE__;

    return 0;
}
