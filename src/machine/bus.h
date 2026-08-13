/*
 * src/machine/bus.h
 *
 * The machine's view of a CPU transaction that the MMU did not satisfy.
 *
 * Today this only observes: Emu68 reports the access, the machine records who
 * made it, and Emu68 then services it exactly as it always did. Routing a
 * transaction to a subsystem is a later question, and giving it a home here is
 * the point of the boundary.
 */

#ifndef BELLATRIX_MACHINE_BUS_H
#define BELLATRIX_MACHINE_BUS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Note a guest access to the classic 24-bit domain.
 *
 * Addresses at or above 0x01000000 are ignored, so the call is safe to place
 * on the whole fault path. size is in bytes, as Emu68 reports it.
 */
void machine_bus_observe(uint32_t address, int size, int write);

/* Print the accesses seen so far, with their counts. */
void machine_bus_report(void);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_MACHINE_BUS_H */
