#ifndef BELLATRIX_RUNTIME_EVENT_H
#define BELLATRIX_RUNTIME_EVENT_H

#include <stdint.h>
#include <stdbool.h>

#define RUNTIME_EVENT_QUEUE_SIZE 256

typedef enum RuntimeEventType {
    RUNTIME_EVENT_NONE = 0,

    /*
     * Video / timing
     */
    RUNTIME_EVENT_VBLANK,
    RUNTIME_EVENT_BEAM,
    RUNTIME_EVENT_COPPER,

    /*
     * Interrupts
     */
    RUNTIME_EVENT_INTREQ,
    RUNTIME_EVENT_IPL_CHANGE,

    /*
     * Audio
     */
    RUNTIME_EVENT_AUDIO_BUFFER,
    RUNTIME_EVENT_MIDI,

    /*
     * Serial / IO
     */
    RUNTIME_EVENT_SERIAL_RX,
    RUNTIME_EVENT_SERIAL_TX,

    /*
     * Floppy / storage
     */
    RUNTIME_EVENT_DISK,

    /*
     * Host side
     */
    RUNTIME_EVENT_INPUT,
    RUNTIME_EVENT_NETWORK,

    /*
     * Runtime
     */
    RUNTIME_EVENT_SHUTDOWN
} RuntimeEventType;

typedef struct RuntimeEvent {
    RuntimeEventType type;

    uint64_t timestamp_cycles;

    uint32_t param0;
    uint32_t param1;

    void *ptr;
} RuntimeEvent;

typedef struct RuntimeEventQueue {
    RuntimeEvent events[RUNTIME_EVENT_QUEUE_SIZE];

    uint32_t head;
    uint32_t tail;
    uint32_t count;

    uint64_t dropped_events;
} RuntimeEventQueue;

void runtime_event_queue_init(RuntimeEventQueue *queue);
void runtime_event_queue_reset(RuntimeEventQueue *queue);

bool runtime_event_push(RuntimeEventQueue *queue,
                        const RuntimeEvent *event);

bool runtime_event_pop(RuntimeEventQueue *queue,
                       RuntimeEvent *event_out);

bool runtime_event_peek(const RuntimeEventQueue *queue,
                        RuntimeEvent *event_out);

bool runtime_event_is_empty(
    const RuntimeEventQueue *queue);

bool runtime_event_is_full(
    const RuntimeEventQueue *queue);

uint32_t runtime_event_count(
    const RuntimeEventQueue *queue);

uint64_t runtime_event_dropped(
    const RuntimeEventQueue *queue);

const char *runtime_event_type_name(
    RuntimeEventType type);

#endif