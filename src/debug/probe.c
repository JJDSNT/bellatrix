#include "debug/probe.h"

#include "support.h"

static const char *probe_evt_name(uint8_t t)
{
    switch ((ProbeEventType)t) {
        case PROBE_EVT_VBL:          return "VBL        ";
        case PROBE_EVT_IPL_RISE:     return "IPL_RISE   ";
        case PROBE_EVT_IPL_DROP:     return "IPL_DROP   ";
        case PROBE_EVT_INTENA_WRITE: return "INTENA_WR  ";
        case PROBE_EVT_INTREQ_SET:   return "INTREQ_SET ";
        case PROBE_EVT_INTREQ_CLR:   return "INTREQ_CLR ";
        case PROBE_EVT_CIA_READ:     return "CIA_READ   ";
        case PROBE_EVT_CIA_WRITE:    return "CIA_WRITE  ";
        case PROBE_EVT_COPPER_MOVE:  return "COP_MOVE   ";
        case PROBE_EVT_COPPER_WAIT:  return "COP_WAIT   ";
        case PROBE_EVT_COPPER_HALT:  return "COP_HALT   ";
        case PROBE_EVT_CPU_STOP:     return "CPU_STOP   ";
        case PROBE_EVT_CPU_EXCEPT:   return "CPU_EXCEPT ";
        case PROBE_EVT_WATCHDOG:     return "WATCHDOG   ";
        case PROBE_EVT_CUSTOM:       return "CUSTOM     ";
        default:                     return "???        ";
    }
}

const char *probe_event_name(uint8_t type)
{
    return probe_evt_name(type);
}

void probe_init(ProbeState *p)
{
    uint32_t i;

    if (!p) {
        return;
    }

    p->head = 0;
    p->paused = false;

    for (i = 0; i < PROBE_BUF_SIZE; i++) {
        p->buf[i].cycle_lo = 0;
        p->buf[i].vpos = 0;
        p->buf[i].type = PROBE_EVT_NONE;
        p->buf[i].reserved = 0;
        p->buf[i].a = 0;
        p->buf[i].b = 0;
    }
}

void probe_reset(ProbeState *p)
{
    probe_init(p);
}

void probe_pause(ProbeState *p)
{
    if (p) {
        p->paused = true;
    }
}

void probe_resume(ProbeState *p)
{
    if (p) {
        p->paused = false;
    }
}

void probe_emit(ProbeState *p,
                ProbeEventType type,
                uint32_t cycle_lo,
                uint16_t vpos,
                uint32_t a,
                uint32_t b)
{
    ProbeEvent *e;
    uint32_t idx;

    if (!p || p->paused) {
        return;
    }

    idx = p->head & PROBE_BUF_MASK;
    e = &p->buf[idx];

    e->cycle_lo = cycle_lo;
    e->vpos = vpos;
    e->type = (uint8_t)type;
    e->reserved = 0;
    e->a = a;
    e->b = b;

    p->head++;
}

void probe_dump(const ProbeState *p, uint32_t last_n)
{
    uint32_t head;
    uint32_t count;
    uint32_t start;
    uint32_t i;

    if (!p) {
        return;
    }

    head = p->head;
    count = (head < PROBE_BUF_SIZE) ? head : PROBE_BUF_SIZE;

    if (last_n > count) {
        last_n = count;
    }

    start = head - last_n;

    kprintf("=== BellatrixProbe ===\n");

    for (i = 0; i < last_n; i++) {
        const ProbeEvent *e;
        uint32_t idx = (start + i) & PROBE_BUF_MASK;

        e = &p->buf[idx];
        if (e->type == PROBE_EVT_NONE) {
            continue;
        }

        kprintf("[PROBE] %08x %03x %s %08x %08x\n",
                e->cycle_lo,
                (unsigned)e->vpos & 0x0FFFu,
                probe_evt_name(e->type),
                e->a,
                e->b);
    }

    kprintf("=== end probe ===\n");
}