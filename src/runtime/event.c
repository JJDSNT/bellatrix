#include "runtime/event.h"

#include <string.h>

void runtime_event_queue_init(RuntimeEventQueue *queue)
{
    if (!queue) {
        return;
    }

    memset(queue, 0, sizeof(*queue));
}

void runtime_event_queue_reset(RuntimeEventQueue *queue)
{
    runtime_event_queue_init(queue);
}

bool runtime_event_is_empty(
    const RuntimeEventQueue *queue)
{
    return !queue || queue->count == 0;
}

bool runtime_event_is_full(
    const RuntimeEventQueue *queue)
{
    return queue &&
           queue->count >= RUNTIME_EVENT_QUEUE_SIZE;
}

uint32_t runtime_event_count(
    const RuntimeEventQueue *queue)
{
    if (!queue) {
        return 0;
    }

    return queue->count;
}

uint64_t runtime_event_dropped(
    const RuntimeEventQueue *queue)
{
    if (!queue) {
        return 0;
    }

    return queue->dropped_events;
}

bool runtime_event_push(RuntimeEventQueue *queue,
                        const RuntimeEvent *event)
{
    if (!queue || !event) {
        return false;
    }

    if (runtime_event_is_full(queue)) {
        queue->dropped_events++;
        return false;
    }

    queue->events[queue->tail] = *event;

    queue->tail =
        (queue->tail + 1u) %
        RUNTIME_EVENT_QUEUE_SIZE;

    queue->count++;

    return true;
}

bool runtime_event_pop(RuntimeEventQueue *queue,
                       RuntimeEvent *event_out)
{
    if (!queue || !event_out) {
        return false;
    }

    if (runtime_event_is_empty(queue)) {
        return false;
    }

    *event_out = queue->events[queue->head];

    queue->head =
        (queue->head + 1u) %
        RUNTIME_EVENT_QUEUE_SIZE;

    queue->count--;

    return true;
}

bool runtime_event_peek(const RuntimeEventQueue *queue,
                        RuntimeEvent *event_out)
{
    if (!queue || !event_out) {
        return false;
    }

    if (runtime_event_is_empty(queue)) {
        return false;
    }

    *event_out = queue->events[queue->head];

    return true;
}

const char *runtime_event_type_name(
    RuntimeEventType type)
{
    switch (type) {
    case RUNTIME_EVENT_NONE:
        return "NONE";

    case RUNTIME_EVENT_VBLANK:
        return "VBLANK";

    case RUNTIME_EVENT_BEAM:
        return "BEAM";

    case RUNTIME_EVENT_COPPER:
        return "COPPER";

    case RUNTIME_EVENT_INTREQ:
        return "INTREQ";

    case RUNTIME_EVENT_IPL_CHANGE:
        return "IPL_CHANGE";

    case RUNTIME_EVENT_AUDIO_BUFFER:
        return "AUDIO_BUFFER";

    case RUNTIME_EVENT_MIDI:
        return "MIDI";

    case RUNTIME_EVENT_SERIAL_RX:
        return "SERIAL_RX";

    case RUNTIME_EVENT_SERIAL_TX:
        return "SERIAL_TX";

    case RUNTIME_EVENT_DISK:
        return "DISK";

    case RUNTIME_EVENT_INPUT:
        return "INPUT";

    case RUNTIME_EVENT_NETWORK:
        return "NETWORK";

    case RUNTIME_EVENT_SHUTDOWN:
        return "SHUTDOWN";

    default:
        return "UNKNOWN";
    }
}