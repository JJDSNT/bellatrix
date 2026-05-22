#include "core/bus.h"
#include "core/machine.h"
#include "memory/memory.h"

#include "chipset/agnus/agnus.h"
#include "chipset/denise/denise.h"
#include "chipset/paula/paula.h"
#include "chipset/paula/paula_interrupt.h"
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
        return bellatrix_chip_read8(&m->memory, offset);

    case BUS_SIZE_WORD:
        return bellatrix_chip_read16(&m->memory, offset);

    case BUS_SIZE_LONG:
        return bellatrix_chip_read32(&m->memory, offset);

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
        bellatrix_chip_write8(&m->memory, offset, (uint8_t)value);
        break;

    case BUS_SIZE_WORD:
        bellatrix_chip_write16(&m->memory, offset, (uint16_t)value);
        break;

    case BUS_SIZE_LONG:
        bellatrix_chip_write32(&m->memory, offset, value);
        break;

    default:
        break;
    }
}

static uint32_t read_rom(BellatrixMachine *m,
                         uint32_t addr,
                         BusAccessSize size)
{
    switch (size) {
    case BUS_SIZE_BYTE:
        return bellatrix_mem_read8(&m->memory, addr);

    case BUS_SIZE_WORD:
        return bellatrix_mem_read16(&m->memory, addr);

    case BUS_SIZE_LONG:
        return bellatrix_mem_read32(&m->memory, addr);

    default:
        return 0xffffffffu;
    }
}

static uint32_t read_custom(BellatrixMachine *m,
                            uint32_t addr,
                            BusAccessSize size)
{
    if (size == BUS_SIZE_LONG) {
        uint32_t hi = read_custom(m, addr,     BUS_SIZE_WORD);
        uint32_t lo = read_custom(m, addr + 2, BUS_SIZE_WORD);
        return (hi << 16) | lo;
    }

    if (size == BUS_SIZE_BYTE) {
        uint32_t word = read_custom(m, addr & ~1u, BUS_SIZE_WORD);
        return (addr & 1u) ? (word & 0xffu) : (word >> 8);
    }

    if (paula_handles_read(&m->paula, addr))
        return paula_read(&m->paula, addr, (unsigned)size);
    if (agnus_handles_read(&m->agnus, addr))
        return agnus_read(&m->agnus, addr, (unsigned)size);
    if (denise_handles_read(&m->denise, addr))
        return denise_read(&m->denise, addr, (unsigned)size);

    return 0xffffffffu;
}

static void write_custom(BellatrixMachine *m,
                         uint32_t addr,
                         uint32_t value,
                         BusAccessSize size)
{
    if (size == BUS_SIZE_LONG) {
        write_custom(m, addr,     value >> 16,     BUS_SIZE_WORD);
        write_custom(m, addr + 2, value & 0xffffu, BUS_SIZE_WORD);
        return;
    }

    if (size == BUS_SIZE_BYTE) {
        uint32_t aligned = addr & ~1u;
        uint32_t old = read_custom(m, aligned, BUS_SIZE_WORD);
        uint32_t next;

        if (addr & 1u)
            next = (old & 0xff00u) | (value & 0x00ffu);
        else
            next = (old & 0x00ffu) | ((value & 0x00ffu) << 8);

        write_custom(m, aligned, next, BUS_SIZE_WORD);
        return;
    }

    if (paula_handles_write(&m->paula, addr))
        paula_write(&m->paula, addr, value, (unsigned)size);
    else if (agnus_handles_write(&m->agnus, addr))
        agnus_write(&m->agnus, addr, value, (unsigned)size);
    else if (denise_handles_write(&m->denise, addr))
        denise_write(&m->denise, addr, value, (unsigned)size);
}

static uint32_t read_cia_a(BellatrixMachine *m,
                           uint32_t addr,
                           BusAccessSize size)
{
    (void)size;
    uint32_t reg = (addr & CIA_MASK) >> 8;
    return cia_read_reg(&m->cia_a, reg & 0x0f);
}

static uint32_t read_cia_b(BellatrixMachine *m,
                           uint32_t addr,
                           BusAccessSize size)
{
    (void)size;
    uint32_t reg = (addr & CIA_MASK) >> 8;
    return cia_read_reg(&m->cia_b, reg & 0x0f);
}

static void write_cia_a(BellatrixMachine *m,
                        uint32_t addr,
                        uint32_t value,
                        BusAccessSize size)
{
    (void)size;
    uint32_t reg = (addr & CIA_MASK) >> 8;
    cia_write_reg(&m->cia_a, reg & 0x0f, (uint8_t)value);
}

static void write_cia_b(BellatrixMachine *m,
                        uint32_t addr,
                        uint32_t value,
                        BusAccessSize size)
{
    (void)size;
    uint32_t reg = (addr & CIA_MASK) >> 8;
    cia_write_reg(&m->cia_b, reg & 0x0f, (uint8_t)value);
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
        uint32_t v = gayle_read(&m->gayle, addr, size);
        /* Data exhausted during a read may set irq_pending (command complete). */
        if (gayle_ide_irq_pending(&m->gayle.ide))
            paula_irq_raise(&m->paula, PAULA_INT_PORTS);
        return v;
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
        /*
         * If the GAYLE IDE controller asserted its interrupt after this write
         * (ATAPI command/data ready), propagate to Paula PORTS (Level 2).
         * AROS's IDE interrupt handler reads $DA9000 to confirm it's IDE.
         */
        if (gayle_ide_irq_pending(&m->gayle.ide))
            paula_irq_raise(&m->paula, PAULA_INT_PORTS);
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
