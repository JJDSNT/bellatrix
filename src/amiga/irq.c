/*
 * Classic Amiga interrupt boundary.
 *
 * Rigel owns INTREQ, INTENA and priority resolution.  This file only
 * publishes Rigel's resolved, level-held IPL to the CPU interface.
 */

#include "amiga/irq.h"

#include "A64.h"
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

/*
 * The interrupt boundary, watched.
 *
 * This is the one thing that changed owner when the chipset moved to a core of
 * its own, and the machine stops inside amigavideo's init -- which is where it
 * installs a VERTB server, a blitter server and a CIA-B TOD vector and then
 * expects them to fire. Guessing between "the level is never raised" and "the
 * level is raised and never reaches the CPU" is a coin toss; the two look
 * identical from outside and have nothing in common as defects.
 *
 * So report both ends, bounded so a running machine is not drowned:
 *
 *   [IRQ] pub    the owner published a new level, with INTENA/INTREQ
 *   [IRQ] ask    Emu68 arbitrated and what it was told
 *   [IRQ] stuck  the same non-zero level asked for repeatedly, which means
 *                the guest is not clearing it -- an interrupt that arrives
 *                and is never serviced looks exactly like one that never
 *                arrives, from the outside
 */
enum { AMIGA_IRQ_REPORTS = 24 };
static uint32_t irq_pub_reports;
static uint32_t irq_ask_reports;
static uint32_t irq_same_asks;
static uint8_t  irq_last_ask;

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
    if (irq_pub_reports < AMIGA_IRQ_REPORTS)
    {
        irq_pub_reports++;
        kprintf("[BELLATRIX:IRQ] pub %u -> %u intena=%04x intreq=%04x\n",
            (unsigned)atomic_load_explicit(&published_ipl, memory_order_relaxed),
            (unsigned)ipl,
            (unsigned)rigel_get_intena(irq_rigel),
            (unsigned)rigel_get_intreq(irq_rigel));
    }

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
    uint8_t ipl = atomic_load_explicit(&published_ipl, memory_order_acquire);

    if (ipl != 0)
    {
        if (ipl == irq_last_ask)
        {
            /*
             * Powers of two, so a level that is never cleared says so at
             * 1, 2, 4 ... rather than once per arbitration for the life of
             * the machine.
             */
            irq_same_asks++;
            if ((irq_same_asks & (irq_same_asks - 1u)) == 0u)
                kprintf("[BELLATRIX:IRQ] stuck level %u asked %u times\n",
                    (unsigned)ipl, (unsigned)irq_same_asks);
        }
        else
        {
            irq_last_ask = ipl;
            irq_same_asks = 1;
            if (irq_ask_reports < AMIGA_IRQ_REPORTS)
            {
                irq_ask_reports++;
                kprintf("[BELLATRIX:IRQ] ask %u\n", (unsigned)ipl);
            }
        }
    }
    else
    {
        irq_last_ask = 0;
        irq_same_asks = 0;
    }

    return ipl;
}
