#ifndef BELLATRIX_MACHINE_MEMORY_H
#define BELLATRIX_MACHINE_MEMORY_H

#include <stdint.h>
#include <amiga/memory_map.h>

uint16_t machine_chip_ram_read16(uint32_t address);
void machine_chip_ram_write16(uint32_t address, uint16_t value);

#endif /* BELLATRIX_MACHINE_MEMORY_H */
