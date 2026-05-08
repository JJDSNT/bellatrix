#include "chipset/cia/cia_interrupt.h"

#include "chipset/cia/cia.h"
#include "chipset/paula/paula.h"
#include "support.h"

void cia_interrupt_sync_irq_line(CIA *cia)
{
    if (!cia->paula)
        return;

    if (cia_irq_pending(cia))
    {
        paula_irq_raise(cia->paula, cia->paula_irq_bit);
        if (!cia->irq_asserted)
        {
            kprintf("[CIA%c-IRQ] raised data=%02x mask=%02x\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B',
                    (unsigned)cia->icr_data,
                    (unsigned)cia->icr_mask);
        }
        cia->irq_asserted = 1u;
    }
    else
    {
        paula_irq_clear(cia->paula, cia->paula_irq_bit);
        if (cia->irq_asserted)
        {
            kprintf("[CIA%c-IRQ] cleared\n",
                    cia->id == CIA_PORT_A ? 'A' : 'B');
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
