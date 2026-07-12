#ifndef BELLATRIX_RUNTIME_POSTED_WRITES_H
#define BELLATRIX_RUNTIME_POSTED_WRITES_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* SPSC ring of chipset writes stamped with emulated time (ISSUE-0049 Fase 2).
 *
 * Producer: the CPU core, lock-free. Consumers: whoever holds the chipset
 * access lock — Core 2 applying entries at their stamps while stepping, or
 * the CPU core force-draining before a synchronous contact. The lock
 * serializes consumers, so head/tail form a plain SPSC pair.
 *
 * The queue itself never blocks: try_push returns false when full and the
 * caller owns the wait policy. That keeps liveness decisions (pause,
 * shutdown, mode change) at the call site, where runtime state is visible —
 * a full queue can always be resolved either by the consumer applying
 * entries or by the producer falling back to the synchronous path. */

#define POSTED_WRITES_CAPACITY 256u

typedef struct PostedWrite {
    uint64_t stamp_cck;
    uint32_t addr;
    uint32_t value;
    uint32_t size;
} PostedWrite;

typedef struct PostedWriteQueue {
    PostedWrite ring[POSTED_WRITES_CAPACITY];
    _Atomic uint32_t head;   /* producer cursor, free-running */
    _Atomic uint32_t tail;   /* consumer cursor, free-running */
} PostedWriteQueue;

typedef void (*PostedWriteApplyFn)(void *ctx, const PostedWrite *write);

/* Not thread-safe against concurrent push/apply; callers quiesce first. */
void posted_writes_reset(PostedWriteQueue *q);

uint32_t posted_writes_depth(const PostedWriteQueue *q);

/* Producer only. False when full — no internal waiting. */
bool posted_writes_try_push(PostedWriteQueue *q, const PostedWrite *write);

/* Earliest pending stamp, or UINT64_MAX when empty. */
uint64_t posted_writes_next_stamp(const PostedWriteQueue *q);

/* Apply entries with stamp <= limit, in order, via `apply`. Consumer side
 * (chipset lock held). Returns the number of entries applied. */
uint32_t posted_writes_apply(PostedWriteQueue *q, uint64_t limit,
                             PostedWriteApplyFn apply, void *ctx);

#endif
