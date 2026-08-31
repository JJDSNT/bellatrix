#ifndef BELLATRIX_AMIGA_CONSOLE_H
#define BELLATRIX_AMIGA_CONSOLE_H

#include <stdint.h>

/*
 * A console sink for a machine with more than one core.
 *
 * Ported from the legacy tree's src/host/raspi3/console_log.c, whose note is
 * the reason it exists: buffering the log "destravou diagnóstico" -- garbled
 * output is the loss of the instrument you are debugging with.
 *
 * Two problems, one shape. Emu68 holds print_lock for the whole of a kprintf,
 * so with direct UART writes a core owns the console for about nine
 * milliseconds per line and nothing else may print -- unaffordable for a core
 * keeping a realtime chipset. And AROS reaches the serial one character per
 * call (krnPutC writes a byte to an address Emu68 leaves unmapped, so every
 * character is a fault and a kprintf), so that lock protects a single
 * character and lines interleave anyway.
 *
 * So: each core assembles its line alone, publishes it whole into a ring of
 * its own -- no producer contends with another -- and one core drains.
 */

/* Install as Emu68's putc override. Call once, before the second core logs. */
void amiga_console_init(void);

/* Drain rings to the UART. Called by the core that owns the console. */
void amiga_console_drain(void);

/* Does not return: the console core's loop. */
void amiga_console_run_on_core(void);

#endif /* BELLATRIX_AMIGA_CONSOLE_H */
