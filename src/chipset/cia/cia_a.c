// src/chipset/cia/cia_a.c

#include "cia.h"

/* ------------------------------------------------------------------------- */
/* CIA-A external pin defaults                                               */
/* ------------------------------------------------------------------------- */

/*
 * CIA-A is typically connected to:
 *   PRA: overlay / LED / floppy status / fire buttons
 *   PRB: parallel port
 *
 * For Bellatrix current stage, default all input lines to inactive/high.
 * This already improves over hardcoding ~DDR in the core, because the glue
 * layer can now override ext_pra/ext_prb with real machine signals later.
 */

void cia_a_apply_defaults(CIA *cia)
{
    if (!cia || cia->id != CIA_PORT_A)
        return;

    cia_set_external_pra(cia, 0xFFu);
    cia_set_external_prb(cia, 0xFFu);
}