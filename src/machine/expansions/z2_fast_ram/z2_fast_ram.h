#ifndef BELLATRIX_EXPANSIONS_Z2_FAST_RAM_H
#define BELLATRIX_EXPANSIONS_Z2_FAST_RAM_H

#include <stdint.h>

#include "cpu/cpu_backend.h"
#include "machine/memory/memory.h"

/* Register RAM backing as a Zorro II memory board. The backing exists on the
 * ARM/host side immediately, but no 68k address responds until Autoconfig
 * invokes the board map callback with the guest-assigned base. */
int bellatrix_z2_fast_ram_register(CpuBackend *cpu_backend,
                                   BellatrixMemory *memory,
                                   uint32_t size_bytes);

#endif
