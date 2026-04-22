#ifndef BELLATRIX_DEBUG_PROBE_H
#define BELLATRIX_DEBUG_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef PROBE_BUF_SIZE
#define PROBE_BUF_SIZE 1024u
#endif

#if (PROBE_BUF_SIZE & (PROBE_BUF_SIZE - 1)) != 0
#error "PROBE_BUF_SIZE must be a power of two"
#endif

#define PROBE_BUF_MASK (PROBE_BUF_SIZE - 1u)

typedef enum ProbeEventType {
    PROBE_EVT_NONE = 0,

    PROBE_EVT_VBL,
    PROBE_EVT_IPL_RISE,
    PROBE_EVT_IPL_DROP,

    PROBE_EVT_INTENA_WRITE,
    PROBE_EVT_INTREQ_SET,
    PROBE_EVT_INTREQ_CLR,

    PROBE_EVT_CIA_READ,
    PROBE_EVT_CIA_WRITE,

    PROBE_EVT_COPPER_MOVE,
    PROBE_EVT_COPPER_WAIT,
    PROBE_EVT_COPPER_HALT,

    PROBE_EVT_CPU_STOP,
    PROBE_EVT_CPU_EXCEPT,

    PROBE_EVT_WATCHDOG,
    PROBE_EVT_CUSTOM
} ProbeEventType;

typedef struct ProbeEvent {
    uint32_t cycle_lo;
    uint16_t vpos;
    uint8_t  type;
    uint8_t  reserved;
    uint32_t a;
    uint32_t b;
} ProbeEvent;

typedef struct ProbeState {
    uint32_t head;
    bool     paused;
    ProbeEvent buf[PROBE_BUF_SIZE];
} ProbeState;

void probe_init(ProbeState *p);
void probe_reset(ProbeState *p);
void probe_pause(ProbeState *p);
void probe_resume(ProbeState *p);

void probe_emit(ProbeState *p,
                ProbeEventType type,
                uint32_t cycle_lo,
                uint16_t vpos,
                uint32_t a,
                uint32_t b);

void probe_dump(const ProbeState *p, uint32_t last_n);

const char *probe_event_name(uint8_t type);

#ifdef __cplusplus
}
#endif

#endif