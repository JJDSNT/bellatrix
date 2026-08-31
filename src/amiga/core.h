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

/* Is the chipset running on a core of its own? */
int amiga_core_owns_chipset(void);

/*
 * Guard Rigel from concurrent access.
 *
 * Held by the chipset core around a step, and by the CPU core around an MMIO
 * transaction. Nothing else may touch Rigel.
 */
void amiga_core_lock_acquire(void);
void amiga_core_lock_release(void);

#endif /* BELLATRIX_AMIGA_CORE_H */
