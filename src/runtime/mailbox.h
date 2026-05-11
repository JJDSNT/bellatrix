#ifndef BELLATRIX_RUNTIME_MAILBOX_H
#define BELLATRIX_RUNTIME_MAILBOX_H

#include <stdint.h>
#include <stdbool.h>

#include "runtime/event.h"

#define RUNTIME_MAILBOX_CAPACITY 256

typedef struct RuntimeMailbox {
    RuntimeEvent queue[RUNTIME_MAILBOX_CAPACITY];

    uint32_t head;
    uint32_t tail;
    uint32_t count;

    uint64_t sent;
    uint64_t received;
    uint64_t dropped;
} RuntimeMailbox;

void runtime_mailbox_init(RuntimeMailbox *mb);
void runtime_mailbox_reset(RuntimeMailbox *mb);

bool runtime_mailbox_send(RuntimeMailbox *mb,
                          const RuntimeEvent *event);

bool runtime_mailbox_recv(RuntimeMailbox *mb,
                          RuntimeEvent *event_out);

bool runtime_mailbox_peek(const RuntimeMailbox *mb,
                          RuntimeEvent *event_out);

bool runtime_mailbox_is_empty(const RuntimeMailbox *mb);
bool runtime_mailbox_is_full(const RuntimeMailbox *mb);

uint32_t runtime_mailbox_count(const RuntimeMailbox *mb);

uint64_t runtime_mailbox_sent(const RuntimeMailbox *mb);
uint64_t runtime_mailbox_received(const RuntimeMailbox *mb);
uint64_t runtime_mailbox_dropped(const RuntimeMailbox *mb);

#endif