#include "machine/bus/zorro_autoconfig.h"
#include "machine/memory/memory.h"

#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/bus/zorro3/zorro3.h"

int bellatrix_zorro_autoconfig_in_window(uint32_t addr)
{
    return bellatrix_z2_config_addr_contains(addr);
}

uint8_t bellatrix_zorro_autoconfig_read8(uint32_t addr)
{
    if (bellatrix_zorro2_has_pending_board())
        return bellatrix_zorro2_config_read8(addr);
    if (bellatrix_zorro3_has_pending_board())
        return bellatrix_zorro3_config_read8(addr);
    return 0xffu;
}

uint16_t bellatrix_zorro_autoconfig_read16(uint32_t addr)
{
    if (bellatrix_zorro2_has_pending_board())
        return bellatrix_zorro2_config_read16(addr);
    if (bellatrix_zorro3_has_pending_board())
        return bellatrix_zorro3_config_read16(addr);
    return 0xffffu;
}

uint32_t bellatrix_zorro_autoconfig_read32(uint32_t addr)
{
    if (bellatrix_zorro2_has_pending_board())
        return bellatrix_zorro2_config_read32(addr);
    if (bellatrix_zorro3_has_pending_board())
        return bellatrix_zorro3_config_read32(addr);
    return 0xffffffffu;
}

void bellatrix_zorro_autoconfig_write8(uint32_t addr, uint8_t value)
{
    if (bellatrix_zorro2_has_pending_board())
        bellatrix_zorro2_config_write8(addr, value);
    else if (bellatrix_zorro3_has_pending_board())
        bellatrix_zorro3_config_write8(addr, value);
}

void bellatrix_zorro_autoconfig_write16(uint32_t addr, uint16_t value)
{
    if (bellatrix_zorro2_has_pending_board())
        bellatrix_zorro2_config_write16(addr, value);
    else if (bellatrix_zorro3_has_pending_board())
        bellatrix_zorro3_config_write16(addr, value);
}

void bellatrix_zorro_autoconfig_write32(uint32_t addr, uint32_t value)
{
    if (bellatrix_zorro2_has_pending_board())
        bellatrix_zorro2_config_write32(addr, value);
    else if (bellatrix_zorro3_has_pending_board())
        bellatrix_zorro3_config_write32(addr, value);
}
