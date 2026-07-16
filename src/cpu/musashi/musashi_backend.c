#include "cpu/musashi/musashi_backend.h"

#include "cpu/cpu_bridge.h"
#include "cpu/direct_region.h"
#include "machine/machine.h"
#include "rigel/rigel_cia.h"
#include "machine/memory/memory.h"

#include "m68k.h"

#ifndef BELLATRIX_MUSASHI_CPU_MODEL
#define BELLATRIX_MUSASHI_CPU_MODEL M68K_CPU_TYPE_68040
#endif

#ifndef BELLATRIX_MUSASHI_LOW_RAM_SIZE
#define BELLATRIX_MUSASHI_LOW_RAM_SIZE BELLATRIX_CHIP_CPU_APERTURE_SIZE
#endif

static BellatrixDirectRegionMap s_direct_regions;

static int musashi_direct_map(void *opaque,
                              const BellatrixDirectRegion *region)
{
    (void)opaque;
    (void)region;
    return 0;
}

static int musashi_direct_unmap(void *opaque,
                                const BellatrixDirectRegion *region)
{
    (void)opaque;
    (void)region;
    return 0;
}

static uint32_t musashi_rom_read_at(const uint8_t *rom_base,
                                    uint32_t offset,
                                    unsigned int size)
{
    const uint8_t *rom = rom_base + offset;

    if (size == 1u) {
        return rom[0];
    }
    if (size == 2u) {
        return ((uint32_t)rom[0] << 8) | (uint32_t)rom[1];
    }
    return ((uint32_t)rom[0] << 24) |
           ((uint32_t)rom[1] << 16) |
           ((uint32_t)rom[2] << 8) |
           (uint32_t)rom[3];
}

static int musashi_overlay_enabled(void)
{
    BellatrixMachine *m = bellatrix_machine_get();
    return m ? bellatrix_memory_overlay_enabled(&m->memory) : 1;
}

static uint32_t musashi_chip_read(const BellatrixMemory *mem,
                                  uint32_t addr,
                                  unsigned int size)
{
    if (size == 1u) {
        return bellatrix_chip_read8(mem, addr);
    }
    if (size == 2u) {
        return bellatrix_chip_read16(mem, addr);
    }
    return bellatrix_chip_read32(mem, addr);
}

static void musashi_chip_write(BellatrixMemory *mem,
                               uint32_t addr,
                               uint32_t value,
                               unsigned int size)
{
    if (size == 1u) {
        bellatrix_chip_write8(mem, addr, (uint8_t)value);
    } else if (size == 2u) {
        bellatrix_chip_write16(mem, addr, (uint16_t)value);
    } else {
        bellatrix_chip_write32(mem, addr, value);
    }
}

static uint32_t musashi_read(uint32_t addr, unsigned int size)
{
    BellatrixMachine *m = bellatrix_machine_get();
    BellatrixMemory *mem = &m->memory;
    uint32_t direct_value;

    /* Musashi applies CPU_ADDRESS_MASK before invoking the memory callback:
     * 68000/68010/68EC020 remain 24-bit, while 68020+/68040 preserve 32 bits.
     * A second mask here would alias Z3 into low Amiga memory. */

    if (musashi_overlay_enabled() &&
        mem->rom &&
        mem->rom_size &&
        addr < BELLATRIX_CHIP_BOOT_SIZE) {
        const uint8_t *overlay_rom = mem->rom_ext && mem->rom_ext_size
            ? mem->rom_ext : mem->rom;
        size_t overlay_size = mem->rom_ext && mem->rom_ext_size
            ? mem->rom_ext_size : mem->rom_size;
        return musashi_rom_read_at(
            overlay_rom, addr & (uint32_t)(overlay_size - 1u), size);
    }

    if (mem->rom &&
        addr >= BELLATRIX_ROM_BASE &&
        addr <= BELLATRIX_ROM_END) {
        return musashi_rom_read_at(mem->rom, addr - BELLATRIX_ROM_BASE, size);
    }

    if (mem->rom_ext &&
        addr >= BELLATRIX_EXT_ROM_BASE &&
        addr <= BELLATRIX_EXT_ROM_END) {
        const uint8_t *p = mem->rom_ext + (addr - BELLATRIX_EXT_ROM_BASE);
        if (size == 1u) return p[0];
        if (size == 2u) return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
    }

    if (mem->chip_ram &&
        addr < BELLATRIX_MUSASHI_LOW_RAM_SIZE &&
        size <= (BELLATRIX_MUSASHI_LOW_RAM_SIZE - addr)) {
        return musashi_chip_read(mem, addr, size);
    }

    if (bellatrix_direct_region_read(&s_direct_regions, addr, size,
                                     &direct_value))
        return direct_value;

    return bellatrix_bridge_cpu_read(addr, size);
}

static void musashi_write(uint32_t addr, uint32_t value, unsigned int size)
{
    BellatrixMachine *m = bellatrix_machine_get();
    BellatrixMemory *mem = &m->memory;

    /* Keep the CPU-model address width selected by Musashi. The bridge still
     * returns open bus for unimplemented 32-bit regions, without low aliases. */

    if ((mem->rom &&
         addr >= BELLATRIX_ROM_BASE &&
         addr <= BELLATRIX_ROM_END) ||
        (mem->rom_ext &&
         addr >= BELLATRIX_EXT_ROM_BASE &&
         addr <= BELLATRIX_EXT_ROM_END)) {
        return;
    }

    if (mem->chip_ram &&
        addr < BELLATRIX_MUSASHI_LOW_RAM_SIZE &&
        size <= (BELLATRIX_MUSASHI_LOW_RAM_SIZE - addr)) {
        musashi_chip_write(mem, addr, value, size);
        return;
    }

    if (bellatrix_direct_region_write(&s_direct_regions, addr, size, value))
        return;

    bellatrix_bridge_cpu_write(addr, value, size);
}

unsigned int m68k_read_memory_8(unsigned int address)
{
    return musashi_read((uint32_t)address, 1u);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    return musashi_read((uint32_t)address, 2u);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    return musashi_read((uint32_t)address, 4u);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    musashi_write((uint32_t)address, value, 1u);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    musashi_write((uint32_t)address, value, 2u);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    musashi_write((uint32_t)address, value, 4u);
}

unsigned int m68k_read_disassembler_8(unsigned int address)
{
    return m68k_read_memory_8(address);
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
    return m68k_read_memory_16(address);
}

unsigned int m68k_read_disassembler_32(unsigned int address)
{
    return m68k_read_memory_32(address);
}

static uint32_t musashi_get_pc(void *ctx)
{
    (void)ctx;
    return (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
}

static void musashi_set_ipl(void *ctx, int level)
{
    (void)ctx;
    m68k_set_irq((unsigned int)level);
}

static void musashi_reset(void *ctx)
{
    (void)ctx;
    m68k_pulse_reset();
}

static int musashi_run(void *ctx, uint32_t cycles)
{
    (void)ctx;
    return m68k_execute((int)cycles);
}

static int musashi_map_direct(void *ctx,
                              const BellatrixDirectRegion *region)
{
    (void)ctx;
    return bellatrix_direct_region_install(&s_direct_regions, region);
}

static int musashi_unmap_direct(void *ctx, uint32_t guest_base, uint32_t size)
{
    (void)ctx;
    return bellatrix_direct_region_remove(&s_direct_regions, guest_base, size);
}

static CpuBackend g_musashi_backend = {
    .ctx = NULL,
    .get_pc = musashi_get_pc,
    .set_ipl = musashi_set_ipl,
    .reset = musashi_reset,
    .run = musashi_run,
    .map_direct = musashi_map_direct,
    .unmap_direct = musashi_unmap_direct,
};

const char *bellatrix_musashi_cpu_model(void)
{
    switch (BELLATRIX_MUSASHI_CPU_MODEL) {
    case M68K_CPU_TYPE_68000:
        return "68000";
    case M68K_CPU_TYPE_68010:
        return "68010";
    case M68K_CPU_TYPE_68EC020:
        return "68ec020";
    case M68K_CPU_TYPE_68020:
        return "68020";
    case M68K_CPU_TYPE_68030:
        return "68030";
    case M68K_CPU_TYPE_68040:
        return "68040";
    default:
        return "unknown";
    }
}

void bellatrix_musashi_backend_init(void)
{
    static const BellatrixDirectRegionBackendOps direct_ops = {
        .map = musashi_direct_map,
        .unmap = musashi_direct_unmap,
    };
    bellatrix_direct_region_map_init(&s_direct_regions, &direct_ops, NULL);
    m68k_init();
    m68k_set_cpu_type(BELLATRIX_MUSASHI_CPU_MODEL);
}

CpuBackend *bellatrix_musashi_backend_get(void)
{
    return &g_musashi_backend;
}
