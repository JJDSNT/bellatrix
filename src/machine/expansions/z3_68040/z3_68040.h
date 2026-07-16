#ifndef BELLATRIX_EXPANSIONS_Z3_68040_H
#define BELLATRIX_EXPANSIONS_Z3_68040_H

#include "cpu/cpu_backend.h"

/* Register the native Emu68 68040 support ROM through Bellatrix's shared
 * Zorro III lifecycle. The board remains invisible until the guest assigns
 * its base through Autoconfig. */
int bellatrix_z3_68040_register(CpuBackend *cpu_backend);

#endif
