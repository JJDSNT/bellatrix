#include "core/bus.h"
#include "core/machine.h"
#include "core/memory.h"
#include "memory/memory.h"

#include "chipset/agnus/agnus.h"
#include "chipset/denise/denise.h"
#include "chipset/paula/paula.h"
#include "chipset/cia/cia.h"
#include "bus/gayle/gayle.h"

#include <string.h>

#define CIAA_BASE          0x00bfe000u
#define CIAB_BASE          0x00bfd000u
#define CIA_MASK           0x00000fffu

#define CUSTOM_BASE        0x00dff000u
#define CUSTOM_END         0x00dfffffu

#define ZORRO2_AUTOCONFIG_BASE 0x00e80000u
#define ZORRO2_AUTOCONFIG_END  0x00efffffu

static bool in_range(uint32_t addr, uint32_t base, uint32_t end)
{
    return addr >= base && addr <= end;
}

void bellatrix_bus_init(BellatrixBus *bus,
                        BellatrixMachine *machine)
{
    if (!bus) {
        return;
    }

    memset(bus, 0, sizeof(*bus));
    bus->machine = machine;
}

void bellatrix_bus_reset(BellatrixBus *bus)
{
    if (!bus) {
        return;
    }

    bus->trace_enabled = false;
}

static uint32_t read_chip_ram(BellatrixMachine *m,
                              uint32_t addr,
                              BusAccessSize size)
{
    uint32_t offset = addr & BELLATRIX_CHIP_RAM_MASK;

    switch (size) {
    case BUS_SIZE_BYTE:
        return memory_read8(&m->memory, offset);

    case BUS_SIZE_WORD:
        return memory_read16_be(&m->memory, offset);

    case BUS_SIZE_LONG:
        return memory_read32_be(&m->memory, offset);

    default:
        return 0xffffffffu;
    }
}

static void write_chip_ram(BellatrixMachine *m,
                           uint32_t addr,
                           uint32_t value,
                           BusAccessSize size)
{
    uint32_t offset = addr & BELLATRIX_CHIP_RAM_MASK;

    switch (size) {
    case BUS_SIZE_BYTE:
        memory_write8(&m->memory, offset, (uint8_t)value);
        break;

    case BUS_SIZE_WORD:
        memory_write16_be(&m->memory, offset, (uint16_t)value);
        break;

    case BUS_SIZE_LONG:
        memory_write32_be(&m->memory, offset, value);
        break;

    default:
        break;
    }
}

static uint32_t read_rom(BellatrixMachine *m,
                         uint32_t addr,
                         BusAccessSize size)
{
    uint32_t offset = addr - BELLATRIX_ROM_BASE;

    switch (size) {
    case BUS_SIZE_BYTE:
        return memory_rom_read8(&m->memory, offset);

    case BUS_SIZE_WORD:
        return memory_rom_read16_be(&m->memory, offset);

    case BUS_SIZE_LONG:
        return memory_rom_read32_be(&m->memory, offset);

    default:
        return 0xffffffffu;
    }
}

static uint32_t read_custom(BellatrixMachine *m,
                            uint32_t addr,
                            BusAccessSize size)
{
    uint32_t reg = addr - CUSTOM_BASE;

    if (size == BUS_SIZE_LONG) {
        uint16_t hi = paula_custom_read16(&m->paula, reg);
        uint16_t lo = paula_custom_read16(&m->paula, reg + 2);
        return ((uint32_t)hi << 16) | lo;
    }

    if (size == BUS_SIZE_BYTE) {
        uint16_t word = paula_custom_read16(&m->paula, reg & ~1u);
        return (addr & 1u) ? (word & 0xffu) : (word >> 8);
    }

    /*
     * A implementação real pode despachar por faixa:
     * Agnus: DMA, bitplanes, copper, blitter
     * Denise: BPLCONx, COLORxx, sprites
     * Paula: INTREQ/INTENA, SERDAT, DSK*, AUD*
     *
     * Se hoje você já tem um dispatcher único em paula/agnus/denise,
     * substitua esta chamada por bellatrix_custom_read16(m, reg).
     */
    return bellatrix_machine_custom_read16(m, reg);
}

static void write_custom(BellatrixMachine *m,
                         uint32_t addr,
                         uint32_t value,
                         BusAccessSize size)
{
    uint32_t reg = addr - CUSTOM_BASE;

    if (size == BUS_SIZE_LONG) {
        bellatrix_machine_custom_write16(m, reg,     (uint16_t)(value >> 16));
        bellatrix_machine_custom_write16(m, reg + 2, (uint16_t)(value & 0xffffu));
        return;
    }

    if (size == BUS_SIZE_BYTE) {
        uint32_t aligned = reg & ~1u;
        uint16_t old = bellatrix_machine_custom_read16(m, aligned);
        uint16_t next;

        if (addr & 1u) {
            next = (old & 0xff00u) | (value & 0x00ffu);
        } else {
            next = (old & 0x00ffu) | ((value & 0x00ffu) << 8);
        }

        bellatrix_machine_custom_write16(m, aligned, next);
        return;
    }

    bellatrix_machine_custom_write16(m, reg, (uint16_t)value);
}

static uint32_t read_cia_a(BellatrixMachine *m,
                           uint32_t addr,
                           BusAccessSize size)
{
    (void)size;
    uint32_t reg = (addr & CIA_MASK) >> 8;
    return cia_read(&m->ciaa, reg & 0x0f);
}

static uint32_t read_cia_b(BellatrixMachine *m,
                           uint32_t addr,
                           BusAccessSize size)
{
    (void)size;
    uint32_t reg = (addr & CIA_MASK) >> 8;
    return cia_read(&m->ciab, reg & 0x0f);
}

static void write_cia_a(BellatrixMachine *m,
                        uint32_t addr,
                        uint32_t value,
                        BusAccessSize size)
{
    (void)size;
    uint32_t reg = (addr & CIA_MASK) >> 8;
    cia_write(&m->ciaa, reg & 0x0f, (uint8_t)value);
}

static void write_cia_b(BellatrixMachine *m,
                        uint32_t addr,
                        uint32_t value,
                        BusAccessSize size)
{
    (void)size;
    uint32_t reg = (addr & CIA_MASK) >> 8;
    cia_write(&m->ciab, reg & 0x0f, (uint8_t)value);
}

uint32_t bellatrix_bus_read(BellatrixBus *bus,
                            uint32_t addr,
                            BusAccessSize size)
{
    if (!bus || !bus->machine) {
        return 0xffffffffu;
    }

    BellatrixMachine *m = bus->machine;
    addr &= 0x00ffffffu;

    if (bellatrix_chip_addr_contains(addr)) {
        return read_chip_ram(m, addr, size);
    }

    if (in_range(addr, CUSTOM_BASE, CUSTOM_END)) {
        return read_custom(m, addr, size);
    }

    if ((addr & 0x00fff000u) == CIAA_BASE) {
        return read_cia_a(m, addr, size);
    }

    if ((addr & 0x00fff000u) == CIAB_BASE) {
        return read_cia_b(m, addr, size);
    }

    if (gayle_owns_address(&m->gayle, addr)) {
        return gayle_read(&m->gayle, addr, size);
    }

    if (in_range(addr, BELLATRIX_ROM_BASE, BELLATRIX_ROM_END)) {
        return read_rom(m, addr, size);
    }

    /*
     * Futuro:
     * Zorro II/III AutoConfig, Fast RAM, RTG, etc.
     */
    return 0xffffffffu;
}

void bellatrix_bus_write(BellatrixBus *bus,
                         uint32_t addr,
                         uint32_t value,
                         BusAccessSize size)
{
    if (!bus || !bus->machine) {
        return;
    }

    BellatrixMachine *m = bus->machine;
    addr &= 0x00ffffffu;

    /*
     * OVL afeta leitura/mapeamento inicial, mas writes baixos devem ir
     * para Chip RAM no modelo clássico.
     */
    if (bellatrix_chip_addr_contains(addr)) {
        write_chip_ram(m, addr, value, size);
        return;
    }

    if (in_range(addr, CUSTOM_BASE, CUSTOM_END)) {
        write_custom(m, addr, value, size);
        return;
    }

    if ((addr & 0x00fff000u) == CIAA_BASE) {
        write_cia_a(m, addr, value, size);
        return;
    }

    if ((addr & 0x00fff000u) == CIAB_BASE) {
        write_cia_b(m, addr, value, size);
        return;
    }

    if (gayle_owns_address(&m->gayle, addr)) {
        gayle_write(&m->gayle, addr, value, size);
        return;
    }

    /*
     * ROM e regiões desconhecidas ignoram escrita.
     */
}

uint8_t bellatrix_bus_read8(BellatrixBus *bus, uint32_t addr)
{
    return (uint8_t)bellatrix_bus_read(bus, addr, BUS_SIZE_BYTE);
}

uint16_t bellatrix_bus_read16(BellatrixBus *bus, uint32_t addr)
{
    return (uint16_t)bellatrix_bus_read(bus, addr, BUS_SIZE_WORD);
}

uint32_t bellatrix_bus_read32(BellatrixBus *bus, uint32_t addr)
{
    return bellatrix_bus_read(bus, addr, BUS_SIZE_LONG);
}

void bellatrix_bus_write8(BellatrixBus *bus, uint32_t addr, uint8_t value)
{
    bellatrix_bus_write(bus, addr, value, BUS_SIZE_BYTE);
}

void bellatrix_bus_write16(BellatrixBus *bus, uint32_t addr, uint16_t value)
{
    bellatrix_bus_write(bus, addr, value, BUS_SIZE_WORD);
}

void bellatrix_bus_write32(BellatrixBus *bus, uint32_t addr, uint32_t value)
{
    bellatrix_bus_write(bus, addr, value, BUS_SIZE_LONG);
}
