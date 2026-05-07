// src/bridge/bellatrix_bridge.c

#include "bridge/bellatrix_bridge.h"

#include "core/machine.h"

uint32_t bellatrix_bridge_normalize_addr(uint32_t addr)
{
    addr &= 0x00FFFFFFu;

    /*
     * Emu68 / ROM code may hit mirrored custom-chip addresses instead of the
     * canonical 0xDFFxxx window. Collapse the observed 0x00C?Fxxx mirror so
     * every CPU adapter sees the same logical register page.
     */
    if ((addr & 0x00F00000u) == 0x00C00000u) {
        if ((addr & 0x0000F000u) == 0x0000F000u)
            addr = 0x00DFF000u | (addr & 0x00000FFFu);
    }

    return addr;
}

uint32_t bellatrix_bridge_cpu_read(uint32_t addr, unsigned int size)
{
    return bellatrix_machine_read(bellatrix_bridge_normalize_addr(addr), size);
}

void bellatrix_bridge_cpu_write(uint32_t addr, uint32_t value, unsigned int size)
{
    bellatrix_machine_write(bellatrix_bridge_normalize_addr(addr), value, size);
}

uint32_t bellatrix_bridge_cpu_access(uint32_t addr,
                                     uint32_t value,
                                     unsigned int size,
                                     int dir)
{
    if (dir == BELLATRIX_BUS_WRITE) {
        bellatrix_bridge_cpu_write(addr, value, size);
        return 0;
    }

    return bellatrix_bridge_cpu_read(addr, size);
}

void bellatrix_bridge_cpu_progress(uint32_t cycles)
{
    bellatrix_machine_advance(cycles);
}

void bellatrix_bridge_cpu_sync_ipl(void)
{
    bellatrix_machine_sync_ipl();
}
