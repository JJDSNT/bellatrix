/*
 * Classic Amiga interrupt boundary.
 *
 * Rigel owns INTREQ, INTENA and priority resolution.  This file only
 * publishes Rigel's resolved, level-held IPL to the CPU interface.
 */

#include "amiga/irq.h"

#include "M68k.h"

extern struct M68KState *__m68k_state;

static RigelContext *irq_rigel;
static uint8_t published_ipl;

void amiga_irq_init(RigelContext *rigel)
{
    irq_rigel = rigel;
    published_ipl = 0;
    amiga_irq_sync();
}

void amiga_irq_sync(void)
{
    uint8_t ipl;

    if (irq_rigel == 0 || __m68k_state == 0)
        return;

    ipl = rigel_get_ipl(irq_rigel);
    if (ipl == published_ipl)
        return;

    /*
     * Reuse Emu68's PiStorm-shaped gate: the byte only says that the Amiga
     * domain is asserted.  Arbitration pulls the authoritative level below.
     */
    __atomic_store_n(&__m68k_state->INTF.IPL, ipl != 0, __ATOMIC_RELEASE);
    published_ipl = ipl;

    /* A newly visible level must wake an m68k STOP waiting in Emu68. */
    if (ipl != 0)
        __asm__ volatile("sev" ::: "memory");
}

unsigned int amiga_irq_get_ipl(void)
{
    if (irq_rigel == 0)
        return 0;

    return rigel_get_ipl(irq_rigel);
}
