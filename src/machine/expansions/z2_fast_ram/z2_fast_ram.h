#ifndef BELLATRIX_EXPANSIONS_Z2_FAST_RAM_H
#define BELLATRIX_EXPANSIONS_Z2_FAST_RAM_H

#include <stdint.h>

/*
 * Zorro II Fast RAM as a self-registering board (machine/bus/board_registry.h).
 * The descriptor drops itself into the board table at link time; this call only
 * sizes it and enables it for Autoconfig. The host backing (BellatrixMemory's
 * fast_ram) and the CPU backend are resolved from globals inside the board's
 * map() at Autoconfig time, so no pointers are threaded through here.
 *
 * size_bytes == 0 leaves the board disabled.
 */
void bellatrix_z2_fast_ram_configure(uint32_t size_bytes);

/* Disable the Fast RAM board (it will not participate in Autoconfig). */
void bellatrix_z2_fast_ram_disable(void);

#endif
