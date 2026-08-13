/*
 * src/machine/bus.h
 *
 * The machine's view of a CPU transaction the MMU did not satisfy.
 *
 * Emu68 reconstructs the access and reports it here; the region table decides
 * what it means. Today no region has an owner, so every classic-domain access
 * is recorded and Emu68 goes on to service it exactly as before -- what
 * changes is what is known, not what happens.
 */

#ifndef BELLATRIX_MACHINE_BUS_H
#define BELLATRIX_MACHINE_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deliver a faulted guest access to the machine.
 *
 * An address in no installed region returns immediately, so this is safe to
 * place on the whole fault path. size is in bytes, as Emu68 reports it.
 */
void machine_bus_access(uint32_t address, int size, int write);

/* Print the accesses seen so far, with their counts. */
void machine_bus_report(void);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_MACHINE_BUS_H */
