#ifndef BELLATRIX_AMIGA_BUS_H
#define BELLATRIX_AMIGA_BUS_H

#include <stdint.h>
#include "machine/region.h"

void amiga_bus_init(void);
extern const MachineRegionOps amiga_bus_ops;

#endif /* BELLATRIX_AMIGA_BUS_H */
