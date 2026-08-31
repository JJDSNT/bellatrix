#ifndef BELLATRIX_AMIGA_BUS_H
#define BELLATRIX_AMIGA_BUS_H

#include <stdint.h>
#include "machine/region.h"

#include <stdint.h>

void amiga_bus_init(void);
extern const MachineRegionOps amiga_bus_ops;

/* The chipset's own core runs this and never returns. */
void amiga_clock_run_on_core(void);

#endif /* BELLATRIX_AMIGA_BUS_H */
