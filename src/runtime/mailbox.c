#include "runtime/mailbox.h"

#include <string.h>

void runtime_mailbox_init(RuntimeMailbox *mb)
{
    if (!mb) {
        return;
    }

    memset(mb, 0, sizeof(*mb));
}

void runtime_mailbox_reset(RuntimeMailbox *mb)
{
    runtime_mailbox_init(mb);
}

bool runtime_mailbox_is_empty(const RuntimeMailbox *mb)
{
    return !mb || mb->count == 0;
}

bool runtime_mailbox_is_full(const RuntimeMailbox *mb)
{
    return mb && mb->count >= RUNTIME_MAILBOX_CAPACITY;
}

uint32_t runtime_mailbox_count(const RuntimeMailbox *mb)
{
    if (!mb) {
        return 0;
    }

    return mb->count;
}

uint64_t runtime_mailbox_sent(const RuntimeMailbox *mb)
{
    if (!mb) {
        return 0;
    }

    return mb->sent;
}

uint64_t runtime_mailbox_received(const RuntimeMailbox *mb)
{
    if (!mb) {
        return 0;
    }

    return mb->received;
}

uint64_t runtime_mailbox_dropped(const RuntimeMailbox *mb)
{
    if (!mb) {
        return 0;
    }

    return mb->dropped;
}

bool runtime_mailbox_send(RuntimeMailbox *mb,
                          const RuntimeEvent *event)
{
    if (!mb || !event) {
        return false;
    }

    if (runtime_mailbox_is_full(mb)) {
        mb->dropped++;
        return false;
    }

    mb->queue[mb->tail] = *event;

    mb->tail =
        (mb->tail + 1u) %
        RUNTIME_MAILBOX_CAPACITY;

    mb->count++;
    mb->sent++;

    return true;
}

bool runtime_mailbox_recv(RuntimeMailbox *mb,
                          RuntimeEvent *event_out)
{
    if (!mb || !event_out) {
        return false;
    }

    if (runtime_mailbox_is_empty(mb)) {
        return false;
    }

    *event_out = mb->queue[mb->head];

    mb->head =
        (mb->head + 1u) %
        RUNTIME_MAILBOX_CAPACITY;

    mb->count--;
    mb->received++;

    return true;
}

bool runtime_mailbox_peek(const RuntimeMailbox *mb,
                          RuntimeEvent *event_out)
{
    if (!mb || !event_out) {
        return false;
    }

    if (runtime_mailbox_is_empty(mb)) {
        return false;
    }

    *event_out = mb->queue[mb->head];

    return true;
}