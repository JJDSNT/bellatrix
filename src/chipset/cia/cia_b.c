// src/chipset/cia/cia_b.c

#include "cia.h"

/* ------------------------------------------------------------------------- */
/* CIA-B external pin defaults                                               */
/* ------------------------------------------------------------------------- */

/*
 * CIA-B is typically connected to:
 *   PRA: serial / parallel status lines
 *   PRB: floppy control lines (STEP, DIR, SIDE, SELx, MTR)
 *
 * For Bellatrix current stage, default all input lines to inactive/high.
 *
 * This keeps the core correct: bits configured as input come from ext_pra /
 * ext_prb, while bits configured as output come from pra / prb.
 *
 * Later, Bellatrix glue can override:
 *   - PRA with serial/parallel status
 *   - PRB if any external device needs to reflect line state
 */

void cia_b_apply_defaults(CIA *cia)
{
    if (!cia || cia->id != CIA_PORT_B)
        return;

    cia_set_external_pra(cia, 0xFFu);
    cia_set_external_prb(cia, 0xFFu);
}