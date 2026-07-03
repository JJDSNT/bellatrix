#include "cpu/musashi/musashi_backend.h"

#include "cpu/cpu_bridge.h"
#include "machine/machine.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "rigel/rigel_cia.h"
#include "machine/memory/memory.h"

#include "m68k.h"

static uint32_t musashi_rom_read_at(const BellatrixMemory *mem,
                                    uint32_t offset,
                                    unsigned int size)
{
    const uint8_t *rom = mem->rom + offset;

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
    struct RigelContext *ctx = bellatrix_machine_rigel_ctx();
    if (!ctx) return 1;
    if (!(rigel_cia_read(ctx, 0u, 0x2u) & 0x01u)) return 1;
    return (rigel_cia_read(ctx, 0u, 0x0u) & 0x01u) ? 1 : 0;
}

/* When the fast RAM autoconfig board is registered, the RAM window must
 * stay silent until the OS assigns it a base — otherwise early memory
 * probes find RAM at 0x200000 and the later AddMemList duplicates the
 * range, corrupting the exec memory list. Without a registered board the
 * window responds unconditionally (legacy harness behaviour). */
static int musashi_fast_ram_visible(void)
{
    if (!bellatrix_zorro2_fast_ram_registered()) return 1;
    return bellatrix_zorro2_fast_ram_configured();
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

static uint32_t musashi_fast_read(const BellatrixMemory *mem,
                                  uint32_t addr,
                                  unsigned int size)
{
    if (size == 1u) {
        return bellatrix_fast_read8(mem, addr);
    }
    if (size == 2u) {
        return bellatrix_fast_read16(mem, addr);
    }
    return bellatrix_fast_read32(mem, addr);
}

static void musashi_fast_write(BellatrixMemory *mem,
                               uint32_t addr,
                               uint32_t value,
                               unsigned int size)
{
    if (size == 1u) {
        bellatrix_fast_write8(mem, addr, (uint8_t)value);
    } else if (size == 2u) {
        bellatrix_fast_write16(mem, addr, (uint16_t)value);
    } else {
        bellatrix_fast_write32(mem, addr, value);
    }
}

static uint32_t musashi_read(uint32_t addr, unsigned int size)
{
    BellatrixMachine *m = bellatrix_machine_get();
    BellatrixMemory *mem = &m->memory;

    addr &= 0x00FFFFFFu;

    if (musashi_overlay_enabled() &&
        mem->rom &&
        mem->rom_size &&
        addr < BELLATRIX_CHIP_BOOT_SIZE) {
        return musashi_rom_read_at(mem,
                                   addr & (uint32_t)(mem->rom_size - 1u),
                                   size);
    }

    if (mem->rom &&
        addr >= BELLATRIX_ROM_BASE &&
        addr <= BELLATRIX_ROM_END) {
        return musashi_rom_read_at(mem, addr - BELLATRIX_ROM_BASE, size);
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

    if (bellatrix_chip_addr_contains(addr)) {
        return musashi_chip_read(mem, addr, size);
    }

    if (mem->fast_ram &&
        mem->fast_ram_size &&
        addr >= BELLATRIX_FAST_RAM_BASE &&
        addr <= BELLATRIX_FAST_RAM_END &&
        musashi_fast_ram_visible()) {
        return musashi_fast_read(mem, addr, size);
    }

    return bellatrix_bridge_cpu_read(addr, size);
}

static void musashi_write(uint32_t addr, uint32_t value, unsigned int size)
{
    BellatrixMachine *m = bellatrix_machine_get();
    BellatrixMemory *mem = &m->memory;

    addr &= 0x00FFFFFFu;

    if ((mem->rom &&
         addr >= BELLATRIX_ROM_BASE &&
         addr <= BELLATRIX_ROM_END) ||
        (mem->rom_ext &&
         addr >= BELLATRIX_EXT_ROM_BASE &&
         addr <= BELLATRIX_EXT_ROM_END) ||
        (musashi_overlay_enabled() &&
         addr < BELLATRIX_CHIP_BOOT_SIZE)) {
        return;
    }

    if (bellatrix_chip_addr_contains(addr)) {
        musashi_chip_write(mem, addr, value, size);
        return;
    }

    if (mem->fast_ram &&
        mem->fast_ram_size &&
        addr >= BELLATRIX_FAST_RAM_BASE &&
        addr <= BELLATRIX_FAST_RAM_END &&
        musashi_fast_ram_visible()) {
        musashi_fast_write(mem, addr, value, size);
        return;
    }

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

static CpuBackend g_musashi_backend = {
    .ctx = NULL,
    .get_pc = musashi_get_pc,
    .set_ipl = musashi_set_ipl,
    .reset = musashi_reset,
    .run = musashi_run,
};

void bellatrix_musashi_backend_init(void)
{
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
}

CpuBackend *bellatrix_musashi_backend_get(void)
{
    return &g_musashi_backend;
}
