/*
 * Classic Amiga interrupt boundary.
 *
 * Rigel owns INTREQ, INTENA and priority resolution.  This file only
 * publishes Rigel's resolved, level-held IPL to the CPU interface.
 */

#include "amiga/irq.h"

#include "M68k.h"

#include <stdatomic.h>

extern struct M68KState *__m68k_state;

static RigelContext *irq_rigel;

/*
 * The level, published rather than fetched.
 *
 * Rigel may be owned by a core of its own (ISSUE-0074), and then only that
 * core may touch it. Both of the readers here run on the CPU core:
 * amiga_irq_get_ipl() is called from Emu68's ExecutionLoop on every interrupt
 * arbitration, and the MMIO path used to sync after every transaction. Both
 * called rigel_get_ipl() -- an unsynchronised read of a chipset another core
 * was stepping, which is a race in the interrupt controller of a machine, and
 * it hung within a second of the chipset being armed.
 *
 * So the owner publishes and the CPU reads a word. This is the same shape as
 * legacy's beam snapshot, and for the same reason: reaching across is what
 * costs, not the work itself.
 */
static _Atomic uint8_t published_ipl;

void amiga_irq_init(RigelContext *rigel)
{
    irq_rigel = rigel;
    published_ipl = 0;
    amiga_irq_sync();
}

/*
 * Called by whoever owns Rigel: the chipset core when there is one, the CPU
 * core when there is not. Never by both.
 */
void amiga_irq_sync(void)
{
    uint8_t ipl;

    if (irq_rigel == 0 || __m68k_state == 0)
        return;

    ipl = rigel_get_ipl(irq_rigel);
    if (ipl == atomic_load_explicit(&published_ipl, memory_order_relaxed))
        return;

    /*
     * Reuse Emu68's PiStorm-shaped gate: the byte only says that the Amiga
     * domain is asserted.  Arbitration pulls the authoritative level below.
     */
    /*
     * The level first, then the gate. A CPU core that sees the gate set must
     * find the level already there; the other order lets it arbitrate against
     * a stale one.
     */
    atomic_store_explicit(&published_ipl, ipl, memory_order_release);
    __atomic_store_n(&__m68k_state->INTF.IPL, ipl != 0, __ATOMIC_RELEASE);

    /* A newly visible level must wake an m68k STOP waiting in Emu68. */
    if (ipl != 0)
        __asm__ volatile("sev" ::: "memory");
}

unsigned int amiga_irq_get_ipl(void)
{
    /*
     * Emu68 calls this from the CPU core while arbitrating an interrupt. It
     * must not touch Rigel: another core may be stepping it. Read what the
     * owner published.
     */
    return atomic_load_explicit(&published_ipl, memory_order_acquire);
}
