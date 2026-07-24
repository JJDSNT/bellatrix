#include "cpu/musashi/musashi_backend.h"

#include "cpu/cpu_bridge.h"
#include "cpu/cpu_bus_policy.h"
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
static const CpuBusPolicy *s_bus_policy;
static int s_run_sync_active;
static uint32_t s_run_sync_published;
static uint32_t s_bus_cycles_prepublished;

static void musashi_sync_cpu_progress(void)
{
    int ran;
    uint32_t delta;

    if (!s_run_sync_active)
        return;

    ran = m68k_cycles_run();
    if (ran <= 0 || (uint32_t)ran <= s_run_sync_published)
        return;

    delta = (uint32_t)ran - s_run_sync_published;
    s_run_sync_published = (uint32_t)ran;
    if (s_bus_cycles_prepublished != 0u) {
        uint32_t covered = delta < s_bus_cycles_prepublished
            ? delta : s_bus_cycles_prepublished;
        delta -= covered;
        s_bus_cycles_prepublished -= covered;
    }
    if (delta == 0u)
        return;
    bellatrix_bridge_cpu_progress(delta);
}

static void musashi_begin_chip_access(unsigned int size)
{
    unsigned int transfers = size == 4u ? 2u : 1u;
    uint32_t waited;
    uint32_t wait_cpu_cycles;

    musashi_sync_cpu_progress();
    if (!s_bus_policy || !s_bus_policy->stalls_on_chip_access)
        return;

    waited = bellatrix_machine_cpu_chip_access(transfers, 2u);

    /* A 68000 word transfer is already included in Musashi's four-clock
     * instruction timing, so only pre-publish it to Rigel. Arbitration waits
     * are additional CPU clocks and must also shorten this timeslice. */
    s_bus_cycles_prepublished += transfers * 4u;
    wait_cpu_cycles = waited * 2u;
    if (wait_cpu_cycles != 0u) {
        m68k_modify_timeslice(-(int)wait_cpu_cycles);
        s_run_sync_published += wait_cpu_cycles;
    }
}

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
        musashi_begin_chip_access(size);
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
        musashi_begin_chip_access(size);
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

    /* Musashi only services pending interrupts at the start of an execute
     * timeslice (or when an instruction writes SR).  An IPL raise arriving
     * mid-slice would otherwise wait until the slice ends — long enough for
     * the guest to Disable() and rescind it (lost preemption, see
     * ISSUE-0026).  End the slice so the IRQ is taken on the next
     * instruction boundary, as real hardware would.
     *
     * ISSUE-0070: this is the ISSUE-0026 fix, which in 2026-07-03 was applied
     * only to tools/harness/musashi_backend.c.  The product carried the
     * original defect until 2026-07-20 — which is why AROS booted past
     * lowlevel.library in the harness and stalled there on the product. */
    if (level > 0)
        m68k_end_timeslice();
}

static void musashi_reset(void *ctx)
{
    (void)ctx;
    m68k_pulse_reset();
}

static int musashi_run(void *ctx, uint32_t cycles)
{
    int used;

    (void)ctx;
    s_run_sync_active = 1;
    s_run_sync_published = 0u;
    used = m68k_execute((int)cycles);
    musashi_sync_cpu_progress();
    s_run_sync_active = 0;
    return used;
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
    .progress_in_run = 1,
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
    s_bus_policy = cpu_bus_policy_by_name(bellatrix_musashi_cpu_model());
    m68k_init();
    m68k_set_cpu_type(BELLATRIX_MUSASHI_CPU_MODEL);
}

CpuBackend *bellatrix_musashi_backend_get(void)
{
    return &g_musashi_backend;
}
