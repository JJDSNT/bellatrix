// src/chipset/cia/cia.c

#include "cia.h"
#include "chipset/cia/cia_interrupt.h"
#include "chipset/cia/cia_ports.h"
#include "chipset/cia/cia_serial.h"
#include "chipset/cia/cia_timers.h"
#include "chipset/paula/paula.h"
#include "support.h"

#include <string.h>

static inline void cia_reset_core_state(CIA *cia)
{
    cia_ports_reset(cia);
    cia->cnt_level = 1u;
    cia->flag_level = 1u;
    cia_serial_reset(cia);

    cia_interrupt_reset(cia);
    cia_timers_reset(cia);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void cia_init(CIA *cia, CIA_ID id)
{
    memset(cia, 0, sizeof(*cia));

    cia->id = id;
    cia->irq_level = (id == CIA_PORT_A) ? 2u : 6u;
    cia->paula_irq_bit = (id == CIA_PORT_A) ? (uint16_t)PAULA_INT_PORTS
                                            : (uint16_t)PAULA_INT_EXTER;
    cia->paula = NULL;

    cia_reset(cia);
}

void cia_reset(CIA *cia)
{
    CIA_ID saved_id = cia->id;
    uint8_t saved_irq = cia->irq_level;
    uint16_t saved_paulabit = cia->paula_irq_bit;
    struct Paula *saved_paula = cia->paula;

    cia_reset_core_state(cia);

    cia->id = saved_id;
    cia->irq_level = saved_irq;
    cia->paula_irq_bit = saved_paulabit;
    cia->paula = saved_paula;

    cia_tod_reset(&cia->tod,
                  (cia->id == CIA_PORT_A) ? CIA_A_TOD_TICKS_PER_INCREMENT : CIA_B_TOD_TICKS_PER_INCREMENT);

    cia_ports_apply_defaults(cia);

    cia_interrupt_sync_irq_line(cia);
}

/* ------------------------------------------------------------------------- */
/* wiring                                                                    */
/* ------------------------------------------------------------------------- */

void cia_attach_paula(CIA *cia, struct Paula *paula)
{
    cia->paula = paula;
    cia_interrupt_sync_irq_line(cia);
}

/* ------------------------------------------------------------------------- */
/* external pins                                                             */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* irq                                                                       */
/* ------------------------------------------------------------------------- */

void cia_step(CIA *cia, uint64_t ticks)
{
    cia_timers_advance_pb_outputs(cia);
    cia_timers_step(cia, ticks);
    cia_tod_step(cia, ticks);
}

/* ------------------------------------------------------------------------- */
/* mmio                                                                      */
/* ------------------------------------------------------------------------- */

uint8_t cia_read_reg(CIA *cia, uint8_t reg)
{
    switch (reg & 0x0Fu)
    {
    case CIA_REG_PRA:
    case CIA_REG_PRB:
    case CIA_REG_DDRA:
    case CIA_REG_DDRB:
    {
        uint8_t v = 0xFFu;
        if (!cia_ports_read_reg(cia, reg, &v))
            return 0xFFu;

        if ((reg & 0x0Fu) == CIA_REG_PRA)
        {
            static uint8_t last_pra_a, last_pra_b;
            uint8_t *last = (cia->id == CIA_PORT_A) ? &last_pra_a : &last_pra_b;
            if (v != *last)
            {
                kprintf("[CIA%c-PRA-R] -> %02x  (pra=%02x ddra=%02x ext=%02x)\n",
                        cia->id == CIA_PORT_A ? 'A' : 'B',
                        (unsigned)v, (unsigned)cia->pra,
                        (unsigned)cia->ddra, (unsigned)cia->ext_pra);
                *last = v;
            }
        }
        else if ((reg & 0x0Fu) == CIA_REG_PRB)
        {
            static uint8_t last_prb_a, last_prb_b;
            uint8_t *last = (cia->id == CIA_PORT_A) ? &last_prb_a : &last_prb_b;
            if (v != *last)
            {
                kprintf("[CIA%c-PRB-R] -> %02x  (prb=%02x ddrb=%02x ext=%02x)\n",
                        cia->id == CIA_PORT_A ? 'A' : 'B',
                        (unsigned)v, (unsigned)cia->prb,
                        (unsigned)cia->ddrb, (unsigned)cia->ext_prb);
                *last = v;
            }
        }

        return v;
    }

    case CIA_REG_TALO:
    case CIA_REG_TAHI:
    case CIA_REG_TBLO:
    case CIA_REG_TBHI:
    case CIA_REG_CRA:
    case CIA_REG_CRB:
    {
        uint8_t v = 0xFFu;
        if (cia_timers_read_reg(cia, reg, &v))
            return v;
        return 0xFFu;
    }

    case CIA_REG_TODLO:
    case CIA_REG_TODMID:
    case CIA_REG_TODHI:
        return cia_tod_read(cia, reg & 0x0Fu);

    case CIA_REG_UNUSED:
        return 0xFFu;

    case CIA_REG_SDR:
    {
        uint8_t v = cia->sdr;
        cia->sdr_full = 0u;
        return v;
    }

    case CIA_REG_ICR:
    {
        uint8_t val = cia_interrupt_read_icr(cia);
        /* log only on value change to avoid flooding tight polling loops */
        kprintf("[CIA%c-ICR-R] returned=%02x (mask=%02x)\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                (unsigned)val,
                (unsigned)cia->icr_mask);
        return val;
    }

    default:
        return 0xFFu;
    }
}

int cia_receive_sdr(CIA *cia, uint8_t val)
{
    return cia_serial_receive_byte(cia, val);
}

void cia_write_reg(CIA *cia, uint8_t reg, uint8_t val)
{
    switch (reg & 0x0Fu)
    {
    case CIA_REG_PRA:
    case CIA_REG_PRB:
    case CIA_REG_DDRA:
    case CIA_REG_DDRB:
        (void)cia_ports_write_reg(cia, reg, val);
        return;

    case CIA_REG_TALO:
    case CIA_REG_TAHI:
    case CIA_REG_TBLO:
    case CIA_REG_TBHI:
    case CIA_REG_CRA:
    case CIA_REG_CRB:
        (void)cia_timers_write_reg(cia, reg, val);
        return;

    case CIA_REG_TODLO:
    case CIA_REG_TODMID:
    case CIA_REG_TODHI:
        cia_tod_write(cia, reg & 0x0Fu, val);
        return;

    case CIA_REG_UNUSED:
        return;

    case CIA_REG_SDR:
        cia_serial_write_sdr(cia, val);
        return;

    case CIA_REG_ICR:
        cia_interrupt_write_icr(cia, val);
        kprintf("[CIA%c-ICR-W] raw=%02x -> mask=%02x\n",
                cia->id == CIA_PORT_A ? 'A' : 'B',
                (unsigned)val,
                (unsigned)cia->icr_mask);
        return;

    default:
        return;
    }
}
