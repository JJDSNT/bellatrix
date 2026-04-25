// tools/harness/musashi_backend.c
//
// Musashi CpuBackend — implements the two machine callbacks using Musashi,
// and provides the memory read/write callbacks that Musashi requires.
//
// Address map served by this dispatcher:
//   0x000000–0x1FFFFF  chip RAM (or ROM overlay at boot)
//   0xE00000–0xEFFFFF  extended ROM (1 MB ROMs only — first 512 KB)
//   0xF80000–0xFFFFFF  standard ROM (Kickstart / AROS second 512 KB)
//   everything else    delegated to bellatrix_machine_read/write (chipset/CIA/RTC)
//
// ROM size → layout:
//   256 KB → single window at 0xFC0000
//   512 KB → single window at 0xF80000
//   1 MB   → first 512 KB at 0xE00000, second 512 KB at 0xF80000

#include "musashi_backend.h"
#include "core/machine.h"
#include "memory/memory.h"
#include "chipset/cia/cia.h"

#include "m68k.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * ROM storage — up to 1 MB
 * ------------------------------------------------------------------------- */

#define HARNESS_ROM_MAX   (1024u * 1024u)

static uint8_t  s_rom[HARNESS_ROM_MAX];
static uint32_t s_rom_size  = 0;

/* Standard ROM window (reset vectors, Kickstart entry) */
static uint32_t s_rom_std_base  = 0xF80000u;  /* standard: 0xF80000 or 0xFC0000 */
static uint32_t s_rom_std_off   = 0;           /* byte offset into s_rom */
static uint32_t s_rom_std_size  = 0;

/* Extended ROM window (1 MB ROMs: first half at 0xE00000) */
static uint32_t s_rom_ext_base  = 0;
static uint32_t s_rom_ext_off   = 0;
static uint32_t s_rom_ext_size  = 0;

void musashi_backend_load_rom(const uint8_t *data, uint32_t size, uint32_t base)
{
    if (size > HARNESS_ROM_MAX) size = HARNESS_ROM_MAX;
    memcpy(s_rom, data, size);
    s_rom_size = size;

    s_rom_ext_base = s_rom_ext_size = 0;

    if (size > 512u * 1024u) {
        /* 1 MB ROM: split extended (0xE00000) + standard (0xF80000) */
        uint32_t half = size / 2u;
        s_rom_ext_base = 0xE00000u;
        s_rom_ext_off  = 0;
        s_rom_ext_size = half;
        s_rom_std_base = 0xF80000u;
        s_rom_std_off  = half;
        s_rom_std_size = half;
    } else {
        /* Single window: use caller-supplied base (or default) */
        s_rom_std_base = base;
        s_rom_std_off  = 0;
        s_rom_std_size = size;
    }
}

/* ---------------------------------------------------------------------------
 * Overlay helper — CIA-A PRA bit 0 controls ROM/RAM at address 0
 * ------------------------------------------------------------------------- */

static int harness_overlay(void)
{
    const BellatrixMachine *m = bellatrix_machine_get();
    /* OVL = output only when ddra bit 0 = 1 (output).  Kickstart sets this
     * early; at reset ddra=0x03 is set by bellatrix_init. */
    if (!(m->cia_a.ddra & 0x01u)) return 1; /* ddra=input → assume overlay on */
    return (m->cia_a.pra & 0x01u) ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Unified address read (big-endian byte lanes)
 * ------------------------------------------------------------------------- */

static uint32_t rom_read_at(uint32_t byte_off, int size)
{
    if (size == 1) return s_rom[byte_off];
    if (size == 2) return ((uint32_t)s_rom[byte_off] << 8) | s_rom[byte_off + 1];
    if (size == 4) return ((uint32_t)s_rom[byte_off    ] << 24) |
                          ((uint32_t)s_rom[byte_off + 1] << 16) |
                          ((uint32_t)s_rom[byte_off + 2] <<  8) |
                           (uint32_t)s_rom[byte_off + 3];
    return 0;
}

/* AROS-LOOP: count SERDATR reads near the TBE poll loop at 0xFE85FA */
static void aros_loop_check(uint32_t addr, uint32_t ret)
{
    if (addr != 0x00DFF018u) return;
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
    if (pc < 0x00FE85E0u || pc > 0x00FE8610u) return;
    static int count = 0;
    count++;
    if (count <= 20 || count % 100000 == 0)
        printf("[AROS-LOOP] count=%-8d pc=%08x SERDATR=%04x TBE=%d\n",
               count, pc, (unsigned)ret, (ret >> 13) & 1);
}

static uint32_t harness_read(uint32_t addr, int size)
{
    addr &= 0x00FFFFFFu;

    /* Standard ROM window (0xF80000 or 0xFC0000) */
    if (s_rom_std_size && addr >= s_rom_std_base &&
        addr < s_rom_std_base + s_rom_std_size)
        return rom_read_at(s_rom_std_off + (addr - s_rom_std_base), size);

    /* Extended ROM window (1 MB ROMs only) */
    if (s_rom_ext_size && addr >= s_rom_ext_base &&
        addr < s_rom_ext_base + s_rom_ext_size)
        return rom_read_at(s_rom_ext_off + (addr - s_rom_ext_base), size);

    /* Chip RAM window */
    if (addr < 0x200000u) {
        /* Overlay: reads from low page redirect to standard ROM */
        if (harness_overlay() && addr < 0x80000u && s_rom_std_size) {
            uint32_t rom_off = s_rom_std_off + (addr & (s_rom_std_size - 1u));
            return rom_read_at(rom_off, size);
        }
        const BellatrixMemory *mem = &bellatrix_machine_get()->memory;
        if (size == 1) return bellatrix_chip_read8 (mem, addr);
        if (size == 2) return bellatrix_chip_read16(mem, addr);
        if (size == 4) return bellatrix_chip_read32(mem, addr);
        return 0;
    }

    /* Chipset / CIA / RTC */
    {
        uint32_t ret = bellatrix_machine_read(addr, (unsigned int)size);
        aros_loop_check(addr, ret);
        return ret;
    }
}

static void harness_write(uint32_t addr, uint32_t value, int size)
{
    addr &= 0x00FFFFFFu;

    /* ROM windows are read-only */
    if (s_rom_std_size && addr >= s_rom_std_base && addr < s_rom_std_base + s_rom_std_size) return;
    if (s_rom_ext_size && addr >= s_rom_ext_base && addr < s_rom_ext_base + s_rom_ext_size) return;

    /* Chip RAM */
    if (addr < 0x200000u) {
        BellatrixMemory *mem = &bellatrix_machine_get()->memory;
        if (size == 1) bellatrix_chip_write8 (mem, addr, (uint8_t)value);
        if (size == 2) bellatrix_chip_write16(mem, addr, (uint16_t)value);
        if (size == 4) bellatrix_chip_write32(mem, addr, value);
        return;
    }

    /* Chipset / CIA / RTC */
    bellatrix_machine_write(addr, value, (unsigned int)size);
}

/* ---------------------------------------------------------------------------
 * Musashi memory callbacks
 * ------------------------------------------------------------------------- */

unsigned int m68k_read_memory_8(unsigned int address)
{
    return harness_read((uint32_t)address, 1);
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    return harness_read((uint32_t)address, 2);
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    return harness_read((uint32_t)address, 4);
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    harness_write((uint32_t)address, value, 1);
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    harness_write((uint32_t)address, value, 2);
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    harness_write((uint32_t)address, value, 4);
}

/* Disassembler read (same as normal reads) */
unsigned int m68k_read_disassembler_8 (unsigned int a) { return m68k_read_memory_8(a);  }
unsigned int m68k_read_disassembler_16(unsigned int a) { return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* ---------------------------------------------------------------------------
 * CpuBackend callbacks
 * ------------------------------------------------------------------------- */

static uint32_t musashi_get_pc(void *ctx)
{
    (void)ctx;
    return (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
}

static void musashi_set_ipl(void *ctx, int level)
{
    (void)ctx;
    if (level > 0)
        printf("[IPL] set_ipl level=%d  pc=0x%08x\n", level,
               (unsigned)m68k_get_reg(NULL, M68K_REG_PC));
    m68k_set_irq((unsigned int)level);
}

static CpuBackend g_musashi_backend = {
    .ctx     = NULL,
    .get_pc  = musashi_get_pc,
    .set_ipl = musashi_set_ipl,
};

CpuBackend *musashi_backend_get(void)
{
    return &g_musashi_backend;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void musashi_backend_init(void)
{
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
}

void musashi_backend_reset(void)
{
    m68k_pulse_reset();
}

int musashi_backend_run(int cycles)
{
    return m68k_execute(cycles);
}
