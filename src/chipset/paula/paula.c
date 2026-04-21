// src/chipset/paula/paula.c
//
// Paula — INTREQ/INTENA ownership and IRQ consolidation.
// Audio DMA and disk I/O are stubs for future phases.

#include "paula.h"
#include "support.h"

/* Custom chip register addresses */
#define REG_INTENAR  0xDFF01Cu
#define REG_INTREQR  0xDFF01Eu
#define REG_INTENA   0xDFF09Au
#define REG_INTREQ   0xDFF09Cu

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void paula_init(Paula *p)
{
    paula_reset(p);
}

void paula_reset(Paula *p)
{
    p->intreq = 0;
    p->intena = 0;
    p->ipl    = 0;
}

/* ---------------------------------------------------------------------------
 * Wiring stubs — pointers stored but not used in this phase.
 * CIA and Agnus notify Paula via paula_irq_raise() instead of polling.
 * ------------------------------------------------------------------------- */

void paula_attach_agnus(Paula *p, struct AgnusState *agnus)
{
    (void)p;
    (void)agnus;
}

void paula_attach_cia_a(Paula *p, struct CIA_State *cia)
{
    (void)p;
    (void)cia;
}

void paula_attach_cia_b(Paula *p, struct CIA_State *cia)
{
    (void)p;
    (void)cia;
}

/* ---------------------------------------------------------------------------
 * IRQ interface
 * ------------------------------------------------------------------------- */

void paula_irq_raise(Paula *p, uint16_t bits)
{
    p->intreq |= (uint16_t)(bits & 0x3FFFu);
}

void paula_irq_clear(Paula *p, uint16_t bits)
{
    p->intreq &= (uint16_t)~(bits & 0x3FFFu);
}

/* ---------------------------------------------------------------------------
 * IPL derivation
 * ------------------------------------------------------------------------- */

uint8_t paula_compute_ipl(const Paula *p)
{
    int master = !!(p->intena & PAULA_INT_MASTER);
    uint16_t pending = (uint16_t)(p->intena & p->intreq & 0x3FFFu);

    if (!master || !pending)
        return 0;

    if (pending & PAULA_INT_EXTER)                                              return 6;
    if (pending & PAULA_INT_RBF)                                                return 5;
    if (pending & (PAULA_INT_AUD0|PAULA_INT_AUD1|PAULA_INT_AUD2|PAULA_INT_AUD3)) return 4;
    if (pending & (PAULA_INT_COPER|PAULA_INT_VERTB|PAULA_INT_BLIT))              return 3;
    if (pending & PAULA_INT_PORTS)                                              return 2;
    if (pending & (PAULA_INT_TBE|PAULA_INT_DSKBLK|PAULA_INT_SOFT))             return 1;

    return 0;
}

/* ---------------------------------------------------------------------------
 * Step — no-op; audio DMA will live here in a future phase
 * ------------------------------------------------------------------------- */

void paula_step(Paula *p, uint32_t ticks)
{
    (void)p;
    (void)ticks;
}

/* ---------------------------------------------------------------------------
 * Bus protocol
 * ------------------------------------------------------------------------- */

int paula_handles_read(const Paula *p, uint32_t addr)
{
    (void)p;
    return addr == REG_INTENAR || addr == REG_INTREQR;
}

int paula_handles_write(const Paula *p, uint32_t addr)
{
    (void)p;
    return addr == REG_INTENA || addr == REG_INTREQ;
}

uint32_t paula_read(Paula *p, uint32_t addr, unsigned int size)
{
    (void)size;
    uint32_t ret = 0;
    switch (addr) {
    case REG_INTENAR: ret = p->intena; break;
    case REG_INTREQR: ret = p->intreq; break;
    default:          break;
    }
    kprintf("[PAULA-R] %s -> %04x  (intena=%04x intreq=%04x)\n",
            addr == REG_INTENAR ? "INTENAR" : "INTREQR",
            (unsigned)ret, (unsigned)p->intena, (unsigned)p->intreq);
    return ret;
}

void paula_write(Paula *p, uint32_t addr, uint32_t value, unsigned int size)
{
    (void)size;
    uint16_t raw = (uint16_t)value;

    switch (addr) {
    case REG_INTENA:
        if (raw & 0x8000u)
            p->intena |= (uint16_t)(raw & 0x7FFFu);
        else
            p->intena &= (uint16_t)~(raw & 0x7FFFu);
        {
            uint16_t pending = (uint16_t)(p->intena & p->intreq & 0x3FFFu);
            kprintf("[PAULA-W] INTENA raw=%04x -> intena=%04x intreq=%04x pending=%04x\n",
                    (unsigned)raw, (unsigned)p->intena,
                    (unsigned)p->intreq, (unsigned)pending);
        }
        break;

    case REG_INTREQ:
        if (raw & 0x8000u)
            p->intreq |= (uint16_t)(raw & 0x3FFFu);
        else
            p->intreq &= (uint16_t)~(raw & 0x3FFFu);
        {
            uint16_t pending = (uint16_t)(p->intena & p->intreq & 0x3FFFu);
            kprintf("[PAULA-W] INTREQ raw=%04x -> intreq=%04x intena=%04x pending=%04x\n",
                    (unsigned)raw, (unsigned)p->intreq,
                    (unsigned)p->intena, (unsigned)pending);
        }
        break;

    default:
        break;
    }
}
