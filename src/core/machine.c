// src/core/machine.c

#include "machine.h"

#include <string.h>

/*
 * Instância global única da máquina.
 * Por enquanto Bellatrix segue singleton no nível da máquina inteira.
 */
static BellatrixMachine g_machine;

/* ------------------------------------------------------------------------- */
/* Accessor                                                                  */
/* ------------------------------------------------------------------------- */

BellatrixMachine *bellatrix_machine_get(void)
{
    return &g_machine;
}

/* ------------------------------------------------------------------------- */
/* Init                                                                      */
/* ------------------------------------------------------------------------- */

void bellatrix_machine_init(struct M68KState *cpu)
{
    memset(&g_machine, 0, sizeof(g_machine));

    g_machine.master_ticks = 0;
    g_machine.cpu = cpu;

    // CIA
    cia_init(&g_machine.cia_a);
    cia_init(&g_machine.cia_b);

    // Agnus
    agnus_init(&g_machine.agnus);
}

/* ------------------------------------------------------------------------- */
/* CIA -> Agnus INTREQ bridge                                                */
/* ------------------------------------------------------------------------- */

static void machine_update_interrupts(BellatrixMachine *m)
{
    /*
     * CIA-A -> INT2 (PORTS)
     * CIA-B -> INT6 (EXTER)
     *
     * INTREQ é nível/latch no Agnus, então re-setar o mesmo bit não quebra
     * o modelo, mas evitamos churn desnecessário de log.
     */

    if (cia_irq_pending(&m->cia_a)) {
        if (!(m->agnus.intreq & INT_PORTS)) {
            agnus_intreq_set(&m->agnus, INT_PORTS);
        }
    }

    if (cia_irq_pending(&m->cia_b)) {
        if (!(m->agnus.intreq & INT_EXTER)) {
            agnus_intreq_set(&m->agnus, INT_EXTER);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Step (master temporal evolution of the machine)                           */
/* ------------------------------------------------------------------------- */

void machine_step(BellatrixMachine *m, uint64_t ticks)
{
    if (!ticks) {
        return;
    }

    m->master_ticks += ticks;

    // ---------------------------------------------------------
    // Deterministic order
    // ---------------------------------------------------------

    // 1. CIA
    cia_step(&m->cia_a, ticks);
    cia_step(&m->cia_b, ticks);

    // 2. Agnus (beam + blitter + VBL)
    agnus_step(&m->agnus, ticks);

    // 3. CIA -> INTREQ integration
    machine_update_interrupts(m);

    // ---------------------------------------------------------
    // FUTURE
    // ---------------------------------------------------------
    // denise_step(&m->denise, ticks);
    // paula_step(&m->paula, ticks);
    // copper_step(...)
}