#include "chipset/cia/cia_interrupt.h"

#include "chipset/cia/cia.h"
#include "chipset/paula/paula.h"
#include "support.h"
#include "debug/core_log.h"

void cia_interrupt_sync_irq_line(CIA *cia)
{
    if (!cia->paula)
        return;

    if (cia_irq_pending(cia))
    {
        paula_irq_raise(cia->paula, cia->paula_irq_bit);
        if (!cia->irq_asserted)
        {
            CORE3_LOG("CIA%c IRQ raised icr_data=%02x icr_mask=%02x",
                      (cia->id == CIA_PORT_A ? 'A' : 'B'),
                      (unsigned)cia->icr_data, (unsigned)cia->icr_mask);
            XCORE_LOG("CIA->PAULA", "CIA%c IRQ -> INTREQ bit %u",
                      (cia->id == CIA_PORT_A ? 'A' : 'B'),
                      (unsigned)cia->paula_irq_bit);
        }
        cia->irq_asserted = 1u;
    }
    else
    {
        paula_irq_clear(cia->paula, cia->paula_irq_bit);
        if (cia->irq_asserted)
        {
            CORE3_LOG("CIA%c IRQ cleared", (cia->id == CIA_PORT_A ? 'A' : 'B'));
        }
        cia->irq_asserted = 0u;
    }
}

void cia_interrupt_reset(CIA *cia)
{
    cia->icr_mask = 0x00u;
    cia->icr_data = 0x00u;
    cia->irq_asserted = 0u;
}

void cia_interrupt_raise(CIA *cia, uint8_t mask)
{
    cia->icr_data |= (uint8_t)(mask & 0x1Fu);
    cia_interrupt_sync_irq_line(cia);
}

uint8_t cia_interrupt_read_icr(CIA *cia)
{
    uint8_t value = cia->icr_data;

    if (cia_irq_pending(cia))
        value |= 0x80u;

    cia->icr_data = 0x00u;
    cia_interrupt_sync_irq_line(cia);
    return value;
}

void cia_interrupt_write_icr(CIA *cia, uint8_t value)
{
    if (value & CIA_ICR_SETCLR)
        cia->icr_mask |= (uint8_t)(value & 0x1Fu);
    else
        cia->icr_mask &= (uint8_t)~(value & 0x1Fu);

    if (cia->id == CIA_PORT_A)
        kprintf("[CIAA-ICR-MASK] write=%02x -> icr_mask=%02x (TA=%d TB=%d)\n",
                (unsigned)value, (unsigned)cia->icr_mask,
                (cia->icr_mask & CIA_ICR_TA) ? 1 : 0,
                (cia->icr_mask & CIA_ICR_TB) ? 1 : 0);

    cia_interrupt_sync_irq_line(cia);
}

int cia_irq_pending(const CIA *cia)
{
    return (cia->icr_data & cia->icr_mask) != 0;
}

uint8_t cia_compute_ipl(const CIA *cia)
{
    return cia_irq_pending(cia) ? cia->irq_level : 0u;
}
