#include "machine/memory.h"

#include <stdint.h>

static int chip_ram_word_valid(uint32_t address)
{
    return address <= BELLATRIX_CHIP_RAM_SIZE - sizeof(uint16_t);
}

uint16_t machine_chip_ram_read16(uint32_t address)
{
    if (!chip_ram_word_valid(address))
        return 0xffffu;

    return *(volatile const uint16_t *)(uintptr_t)
        (BELLATRIX_CHIP_RAM_BASE + address);
}

void machine_chip_ram_write16(uint32_t address, uint16_t value)
{
    if (!chip_ram_word_valid(address))
        return;

    *(volatile uint16_t *)(uintptr_t)
        (BELLATRIX_CHIP_RAM_BASE + address) = value;
}
