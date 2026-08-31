/*
 * src/machine/bus.h
 *
 * The machine's view of a CPU transaction the MMU did not satisfy.
 *
 * Emu68 reconstructs the access and reports it here; the region table decides
 * whether a registered provider serves it. Unowned regions are still observed
 * and returned to Emu68 unchanged.
 */

#ifndef BELLATRIX_MACHINE_BUS_H
#define BELLATRIX_MACHINE_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return nonzero when a registered provider completed the transaction.
 * Otherwise Emu68 continues through its ordinary memory/open-bus path.
 */
int machine_bus_read(uint32_t address, int size, uint64_t *value);
int machine_bus_write(uint32_t address, int size, uint64_t value);

/* Print the accesses seen so far, with their counts. */
void machine_bus_report(void);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_MACHINE_BUS_H */
