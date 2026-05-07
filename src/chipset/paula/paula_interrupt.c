#include "chipset/paula/paula_interrupt.h"

#include <string.h>

static uint8_t paula_interrupt_compute_ipl(
    const PaulaInterrupt *irq)
{
    /*
     * Enabled pending interrupts only.
     */

    uint16_t pending =
        irq->intreq &
        irq->intena;

    /*
     * Master enable must be set.
     */

    if (!(irq->intena & PAULA_INT_INTEN)) {
        return 0;
    }

    /*
     * Match the current Bellatrix Paula priority model:
     * L6 EXTER
     * L5 DSKSYN/RBF
     * L4 AUDx
     * L3 COPER/VERTB/BLIT
     * L2 PORTS
     * L1 TBE/DSKBLK/SOFT
     */

    if (pending & PAULA_INT_EXTER) {
        return 6;
    }

    if (pending &
        (PAULA_INT_DSKSYN |
         PAULA_INT_RBF))
    {
        return 5;
    }

    if (pending &
        (PAULA_INT_AUD0 |
         PAULA_INT_AUD1 |
         PAULA_INT_AUD2 |
         PAULA_INT_AUD3))
    {
        return 4;
    }

    if (pending &
        (PAULA_INT_BLIT |
         PAULA_INT_VERTB |
         PAULA_INT_COPER))
    {
        return 3;
    }

    if (pending & PAULA_INT_PORTS) {
        return 2;
    }

    if (pending &
        (PAULA_INT_TBE |
         PAULA_INT_DSKBLK |
         PAULA_INT_SOFT))
    {
        return 1;
    }

    return 0;
}

static void paula_interrupt_publish_ipl(
    PaulaInterrupt *irq,
    uint8_t new_ipl)
{
    if (irq->ipl == new_ipl) {
        return;
    }

    irq->ipl = new_ipl;

    if (irq->ipl_change_cb) {
        irq->ipl_change_cb(
            irq->opaque,
            new_ipl);
    }
}

void paula_interrupt_init(PaulaInterrupt *irq)
{
    if (!irq) {
        return;
    }

    memset(irq, 0, sizeof(*irq));

    paula_interrupt_reset(irq);
}

void paula_interrupt_reset(PaulaInterrupt *irq)
{
    if (!irq) {
        return;
    }

    irq->intena = 0;
    irq->intreq = 0;
    irq->ipl = 0;
}

void paula_interrupt_write_intena(
    PaulaInterrupt *irq,
    uint16_t value)
{
    if (!irq) {
        return;
    }

    uint16_t mask =
        value & 0x7fffu;

    if (value & PAULA_INT_SETCLR) {
        irq->intena |= mask;
    } else {
        irq->intena &= ~mask;
    }

    paula_interrupt_update(irq);
}

void paula_interrupt_write_intreq(
    PaulaInterrupt *irq,
    uint16_t value)
{
    if (!irq) {
        return;
    }

    uint16_t mask =
        value & 0x7fffu;

    if (value & PAULA_INT_SETCLR) {
        irq->intreq |= mask;
    } else {
        irq->intreq &= ~mask;
    }

    paula_interrupt_update(irq);
}

uint16_t paula_interrupt_read_intena(
    const PaulaInterrupt *irq)
{
    if (!irq) {
        return 0xffffu;
    }

    return irq->intena;
}

uint16_t paula_interrupt_read_intreq(
    const PaulaInterrupt *irq)
{
    if (!irq) {
        return 0xffffu;
    }

    return irq->intreq;
}

void paula_interrupt_raise(
    PaulaInterrupt *irq,
    uint16_t mask)
{
    if (!irq) {
        return;
    }

    irq->intreq |=
        (mask & 0x7fffu);

    paula_interrupt_update(irq);
}

void paula_interrupt_clear(
    PaulaInterrupt *irq,
    uint16_t mask)
{
    if (!irq) {
        return;
    }

    irq->intreq &=
        ~(mask & 0x7fffu);

    paula_interrupt_update(irq);
}

void paula_interrupt_update(
    PaulaInterrupt *irq)
{
    if (!irq) {
        return;
    }

    uint8_t new_ipl =
        paula_interrupt_compute_ipl(irq);

    paula_interrupt_publish_ipl(
        irq,
        new_ipl);
}

uint8_t paula_interrupt_current_ipl(
    const PaulaInterrupt *irq)
{
    if (!irq) {
        return 0;
    }

    return irq->ipl;
}

bool paula_interrupt_pending(
    const PaulaInterrupt *irq,
    uint16_t mask)
{
    if (!irq) {
        return false;
    }

    return (irq->intreq & mask) != 0;
}
