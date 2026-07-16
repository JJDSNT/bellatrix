#ifndef BELLATRIX_BUS_ZORRO_AUTOCONFIG_H
#define BELLATRIX_BUS_ZORRO_AUTOCONFIG_H

#include <stdint.h>

int bellatrix_zorro_autoconfig_in_window(uint32_t addr);

uint8_t  bellatrix_zorro_autoconfig_read8(uint32_t addr);
uint16_t bellatrix_zorro_autoconfig_read16(uint32_t addr);
uint32_t bellatrix_zorro_autoconfig_read32(uint32_t addr);

void bellatrix_zorro_autoconfig_write8(uint32_t addr, uint8_t value);
void bellatrix_zorro_autoconfig_write16(uint32_t addr, uint16_t value);
void bellatrix_zorro_autoconfig_write32(uint32_t addr, uint32_t value);

#endif
