#ifndef BELLATRIX_EMU68_DIRECT_REGION_H
#define BELLATRIX_EMU68_DIRECT_REGION_H

#include "cpu/direct_region.h"

void bellatrix_emu68_direct_region_init(void);
int bellatrix_emu68_direct_region_install(const BellatrixDirectRegion *region);
int bellatrix_emu68_direct_region_remove(uint32_t guest_base, uint32_t size);

#endif
