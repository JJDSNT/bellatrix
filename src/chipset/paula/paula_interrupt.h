#ifndef BELLATRIX_PAULA_INTERRUPT_H
#define BELLATRIX_PAULA_INTERRUPT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Amiga interrupt bit layout.
 */

#define PAULA_INT_TBE      0x0001u
#define PAULA_INT_DSKBLK   0x0002u
#define PAULA_INT_SOFT     0x0004u
#define PAULA_INT_PORTS    0x0008u
#define PAULA_INT_COPER    0x0010u
#define PAULA_INT_VERTB    0x0020u
#define PAULA_INT_BLIT     0x0040u
#define PAULA_INT_AUD0     0x0080u
#define PAULA_INT_AUD1     0x0100u
#define PAULA_INT_AUD2     0x0200u
#define PAULA_INT_AUD3     0x0400u
#define PAULA_INT_RBF      0x0800u
#define PAULA_INT_DSKSYN   0x1000u
#define PAULA_INT_EXTER    0x2000u

#define PAULA_INT_INTEN    0x4000u
#define PAULA_INT_SETCLR   0x8000u

typedef struct PaulaInterrupt {
    /*
     * INTENA/INTREQ internal registers.
     */
    uint16_t intena;
    uint16_t intreq;

    /*
     * Published IPL level.
     */
    uint8_t ipl;

    /*
     * Optional callback.
     */
    void *opaque;

    void (*ipl_change_cb)(
        void *opaque,
        uint8_t new_ipl);
} PaulaInterrupt;

void paula_interrupt_init(PaulaInterrupt *irq);

void paula_interrupt_reset(PaulaInterrupt *irq);

/*
 * INTENA / INTREQ access.
 */

void paula_interrupt_write_intena(
    PaulaInterrupt *irq,
    uint16_t value);

void paula_interrupt_write_intreq(
    PaulaInterrupt *irq,
    uint16_t value);

uint16_t paula_interrupt_read_intena(
    const PaulaInterrupt *irq);

uint16_t paula_interrupt_read_intreq(
    const PaulaInterrupt *irq);

/*
 * Direct source manipulation.
 */

void paula_interrupt_raise(
    PaulaInterrupt *irq,
    uint16_t mask);

void paula_interrupt_clear(
    PaulaInterrupt *irq,
    uint16_t mask);

/*
 * Consolidation.
 */

void paula_interrupt_update(
    PaulaInterrupt *irq);

uint8_t paula_interrupt_current_ipl(
    const PaulaInterrupt *irq);

bool paula_interrupt_pending(
    const PaulaInterrupt *irq,
    uint16_t mask);

#endif