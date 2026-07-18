#include "machine/bus/zorro_autoconfig.h"

#include "machine/memory/memory.h"   /* bellatrix_z2_config_addr_contains */

int bellatrix_zorro_autoconfig_in_window(uint32_t addr)
{
    return bellatrix_z2_config_addr_contains(addr);
}
