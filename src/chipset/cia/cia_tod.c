// src/chipset/cia/cia_tod.c

#include "cia.h"
#include "chipset/paula/paula.h"

/* ------------------------------------------------------------------------- */
/* internal helpers                                                          */
/* ------------------------------------------------------------------------- */

static inline uint8_t cia_tod_lo(uint32_t v)
{
    return (uint8_t)(v & 0xFFu);
}

static inline uint8_t cia_tod_mid(uint32_t v)
{
    return (uint8_t)((v >> 8) & 0xFFu);
}

static inline uint8_t cia_tod_hi(uint32_t v)
{
    return (uint8_t)((v >> 16) & 0xFFu);
}

static inline void cia_tod_raise_alarm(CIA *cia)
{
    /*
     * Mirror the small helper behavior from cia.c without exporting a new
     * internal symbol. This keeps TOD self-contained.
     */
    cia->icr_data |= CIA_ICR_ALRM;

    if (cia->paula) {
        if (cia_irq_pending(cia)) {
            paula_irq_raise(cia->paula, cia->paula_irq_bit);
            cia->irq_asserted = 1u;
        } else {
            paula_irq_clear(cia->paula, cia->paula_irq_bit);
            cia->irq_asserted = 0u;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void cia_tod_reset(CIA_TOD_State *tod, uint32_t ticks_per_inc)
{
    tod->counter       = 0x000000u;
    tod->alarm         = 0x00FFFFu;
    tod->latch         = 0x000000u;
    tod->latched       = false;
    tod->subticks      = 0u;
    tod->ticks_per_inc = ticks_per_inc;
}

/* ------------------------------------------------------------------------- */
/* stepping                                                                  */
/* ------------------------------------------------------------------------- */

static void cia_tod_increment(CIA *cia, uint32_t increments)
{
    while (increments-- > 0) {
        cia->tod.counter = (cia->tod.counter + 1u) & 0x00FFFFFFu;

        if (cia->tod.counter == cia->tod.alarm)
            cia_tod_raise_alarm(cia);
    }
}

void cia_tod_step(CIA *cia, uint64_t ticks)
{
    if (ticks == 0 || cia->tod.ticks_per_inc == 0)
        return;

    cia->tod.subticks += (uint32_t)ticks;

    if (cia->tod.subticks >= cia->tod.ticks_per_inc) {
        uint32_t inc = cia->tod.subticks / cia->tod.ticks_per_inc;
        cia->tod.subticks %= cia->tod.ticks_per_inc;
        cia_tod_increment(cia, inc);
    }
}

void cia_tod_pulse(CIA *cia, uint32_t pulses)
{
    if (!cia || pulses == 0)
        return;

    cia_tod_increment(cia, pulses);
}

/* ------------------------------------------------------------------------- */
/* register access                                                           */
/* ------------------------------------------------------------------------- */

uint8_t cia_tod_read(CIA *cia, uint8_t reg)
{
    switch (reg) {

        case CIA_REG_TODLO:
            if (cia->tod.latched) {
                uint8_t v = cia_tod_lo(cia->tod.latch);
                cia->tod.latched = false;
                return v;
            }
            return cia_tod_lo(cia->tod.counter);

        case CIA_REG_TODMID:
            if (cia->tod.latched)
                return cia_tod_mid(cia->tod.latch);
            return cia_tod_mid(cia->tod.counter);

        case CIA_REG_TODHI:
            /*
             * Latch current TOD on high-byte read.
             */
            cia->tod.latch   = cia->tod.counter;
            cia->tod.latched = true;
            return cia_tod_hi(cia->tod.counter);

        default:
            return 0xFFu;
    }
}

void cia_tod_write(CIA *cia, uint8_t reg, uint8_t val)
{
    uint32_t *target = (cia->crb & CIA_CRB_ALARM) ? &cia->tod.alarm
                                                  : &cia->tod.counter;

    switch (reg) {

        case CIA_REG_TODLO:
            *target = (*target & 0xFFFF00u) | (uint32_t)val;
            return;

        case CIA_REG_TODMID:
            *target = (*target & 0xFF00FFu) | ((uint32_t)val << 8);
            return;

        case CIA_REG_TODHI:
            *target = (*target & 0x00FFFFu) | ((uint32_t)val << 16);
            return;

        default:
            return;
    }
}
