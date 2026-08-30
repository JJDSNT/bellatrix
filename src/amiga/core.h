#ifndef BELLATRIX_AMIGA_CORE_H
#define BELLATRIX_AMIGA_CORE_H

/*
 * Hand a core to the chipset. See src/amiga/core.c and
 * AI_context/issues/ISSUE-0074.md.
 */

/* Called from the CPU core during init, before the secondary core arrives. */
void amiga_core_enable(void);

/* Did the secondary core reach us? Diagnostic; not a synchronisation point. */
int amiga_core_arrived(void);

#endif /* BELLATRIX_AMIGA_CORE_H */
