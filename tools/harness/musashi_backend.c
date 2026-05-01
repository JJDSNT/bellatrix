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

static void aros_wait2_check(uint32_t addr, uint32_t ret)
{
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
    if (pc < 0x00FE9800u || pc > 0x00FE9820u) return;
    static int count = 0;
    count++;
    if (count <= 40 || count % 100000 == 0)
        printf("[AROS-WAIT2] count=%-8d pc=%08x addr=%06x ret=%08x\n",
               count, pc, addr & 0x00FFFFFFu, ret);
}

static int harness_watch_gfx_pc(uint32_t pc)
{
    if (pc >= 0x00FC17C0u && pc <= 0x00FC18F0u)
        return 1;
    if (pc >= 0x00FC9C00u && pc <= 0x00FC9E40u)
        return 1;
    if (pc >= 0x00FCAC80u && pc <= 0x00FCAE40u)
        return 1;
    if (pc >= 0x00FCC980u && pc <= 0x00FCD620u)
        return 1;
    if (pc >= 0x00FC6300u && pc <= 0x00FC6500u)
        return 1;
    if (pc >= 0x00FE8800u && pc <= 0x00FE8848u)
        return 1;
    if (pc >= 0x00FCA480u && pc <= 0x00FCA568u)
        return 1;
    return 0;
}

static int harness_watch_gfx_addr(uint32_t addr)
{
    addr &= 0x00FFFFFFu;

    if (addr >= 0x0000A4C0u && addr <= 0x0000A580u)
        return 1;
    if (addr >= 0x0000A572u && addr <= 0x0000A8C0u)
        return 1;
    if (addr >= 0x0000C4B2u && addr <= 0x0000C800u)
        return 1;

    return 0;
}

static int harness_is_a4d0_abort_pc(uint32_t pc)
{
    switch (pc)
    {
        case 0x00FCC992u:
        case 0x00FCC9E8u:
        case 0x00FCCB3Au:
        case 0x00FCCBF2u:
        case 0x00FCCF98u:
        case 0x00FCA660u:
        case 0x00FCD5BCu:
            return 1;
        default:
            return 0;
    }
}

static uint32_t harness_chip_read(uint32_t addr, int size)
{
    const BellatrixMemory *mem = &bellatrix_machine_get()->memory;

    if (size == 1) return bellatrix_chip_read8(mem, addr);
    if (size == 2) return bellatrix_chip_read16(mem, addr);
    return bellatrix_chip_read32(mem, addr);
}

static void harness_dump_regs(void)
{
    printf("[A4D0-REGS] SR=%04x"
           " D0=%08x D1=%08x D2=%08x D3=%08x D4=%08x D5=%08x D6=%08x D7=%08x"
           " A0=%08x A1=%08x A2=%08x A3=%08x A4=%08x A5=%08x A6=%08x A7=%08x\n",
           (unsigned)m68k_get_reg(NULL, M68K_REG_SR),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D0),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D1),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D2),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D3),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D4),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D5),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D6),
           (unsigned)m68k_get_reg(NULL, M68K_REG_D7),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A0),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A1),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A2),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A3),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A4),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A5),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A6),
           (unsigned)m68k_get_reg(NULL, M68K_REG_A7));
}

static void harness_dump_disasm(const char *tag, uint32_t pc)
{
    char buff[256];
    unsigned int ppc = (unsigned int)m68k_get_reg(NULL, M68K_REG_PPC);

    m68k_disassemble(buff, pc, M68K_CPU_TYPE_68000);
    printf("[A4D0-%s] PC %08x: %s\n", tag, (unsigned)pc, buff);

    if (ppc && ppc != pc)
    {
        m68k_disassemble(buff, ppc, M68K_CPU_TYPE_68000);
        printf("[A4D0-%s] PPC %08x: %s\n", tag, ppc, buff);
    }
}

static void harness_dump_callers(void)
{
    uint32_t sp = (uint32_t)m68k_get_reg(NULL, M68K_REG_A7) & 0x00FFFFFFu;
    uint32_t ret0 = 0;
    uint32_t ret1 = 0;
    uint32_t ret2 = 0;

    if (sp + 12u < bellatrix_machine_get()->memory.chip_ram_size)
    {
        ret0 = harness_chip_read(sp + 0u, 4);
        ret1 = harness_chip_read(sp + 4u, 4);
        ret2 = harness_chip_read(sp + 8u, 4);
    }

    printf("[A4D0-CALLER] A7=%08x RET0=%08x RET1=%08x RET2=%08x\n",
           (unsigned)sp,
           (unsigned)ret0,
           (unsigned)ret1,
           (unsigned)ret2);
}

static void harness_dump_a4d0_state(uint32_t pc)
{
    uint32_t addr;

    printf("[A4D0-DUMP] pc=%08x range=00a4c0..00a560\n", (unsigned)pc);
    for (addr = 0x0000A4C0u; addr <= 0x0000A560u; addr += 0x10u)
    {
        printf("[A4D0-DUMP] %06x: %08x %08x %08x %08x\n",
               (unsigned)addr,
               (unsigned)harness_chip_read(addr + 0x0u, 4),
               (unsigned)harness_chip_read(addr + 0x4u, 4),
               (unsigned)harness_chip_read(addr + 0x8u, 4),
               (unsigned)harness_chip_read(addr + 0xCu, 4));
    }

    printf("[A4D0-DUMP] a542.long=%08x a542.word=%04x a552=%08x a556=%08x a4d0.word=%04x a4d0.long=%08x\n",
           (unsigned)harness_chip_read(0x0000A542u, 4),
           (unsigned)harness_chip_read(0x0000A542u, 2),
           (unsigned)harness_chip_read(0x0000A552u, 4),
           (unsigned)harness_chip_read(0x0000A556u, 4),
           (unsigned)harness_chip_read(0x0000A4D0u, 2),
           (unsigned)harness_chip_read(0x0000A4D0u, 4));
}

static void harness_watch_a4d0(const char *tag, uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    if ((addr & 0x00FFFFFFu) != 0x0000A4D0u)
        return;

    printf("[A4D0-%s] pc=%08x addr=%06x size=%d val=%08x\n",
           tag,
           (unsigned)pc,
           (unsigned)addr,
           size,
           (unsigned)value);
    harness_dump_disasm(tag, pc);
    harness_dump_regs();
    harness_dump_callers();

    if (strstr(tag, "-R") != NULL && value == 0 && harness_is_a4d0_abort_pc(pc))
        harness_dump_a4d0_state(pc);
}

static void harness_watch_rw(const char *tag, uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    harness_watch_a4d0(tag, pc, addr, size, value);

    if (!harness_watch_gfx_pc(pc) && !harness_watch_gfx_addr(addr))
        return;

    printf("[%s] pc=%08x addr=%08x size=%d val=%08x\n",
           tag,
           (unsigned)pc,
           (unsigned)(addr & 0x00FFFFFFu),
           size,
           (unsigned)value);
}

static void harness_probe_happy_builder(void)
{
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

    if (pc < 0x00FCA484u || pc > 0x00FCA568u)
        return;

    if (pc == 0x00FCA484u || pc == 0x00FCA4B0u || pc == 0x00FCA4C6u ||
        pc == 0x00FCA4CCu || pc == 0x00FCA4F0u || pc == 0x00FCA528u ||
        pc == 0x00FCA52Cu || pc == 0x00FCA556u)
    {
        printf("[HH-BUILD] pc=%08x A1=%08x A2=%08x A3=%08x "
               "D0=%08x D1=%08x D2=%08x D3=%08x D4=%08x "
               "a2+0e=%04x a2+10=%04x a2+11=%02x a2+13=%02x a2+18=%04x "
               "a3+02=%04x a3+04=%04x\n",
               (unsigned)pc,
               (unsigned)m68k_get_reg(NULL, M68K_REG_A1),
               (unsigned)m68k_get_reg(NULL, M68K_REG_A2),
               (unsigned)m68k_get_reg(NULL, M68K_REG_A3),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D0),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D1),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D2),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D3),
               (unsigned)m68k_get_reg(NULL, M68K_REG_D4),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x0Eu) & 0x00FFFFFFu, 2),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x10u) & 0x00FFFFFFu, 2),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x11u) & 0x00FFFFFFu, 1),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x13u) & 0x00FFFFFFu, 1),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A2)) + 0x18u) & 0x00FFFFFFu, 2),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A3)) + 0x02u) & 0x00FFFFFFu, 2),
               (unsigned)harness_chip_read((((uint32_t)m68k_get_reg(NULL, M68K_REG_A3)) + 0x04u) & 0x00FFFFFFu, 2));
    }
}

static void harness_instr_hook(unsigned int pc)
{
    (void)pc;
    harness_probe_happy_builder();
}

static uint32_t harness_read(uint32_t addr, int size)
{
    addr &= 0x00FFFFFFu;
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
    uint32_t ret = 0;

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
        if (size == 1) ret = bellatrix_chip_read8(mem, addr);
        else if (size == 2) ret = bellatrix_chip_read16(mem, addr);
        else if (size == 4) ret = bellatrix_chip_read32(mem, addr);
        harness_watch_rw("WATCH-BUS-R", pc, addr, size, ret);
        return ret;
    }

    /* Chipset / CIA / RTC */
    ret = bellatrix_machine_read(addr, (unsigned int)size);
    harness_watch_rw("WATCH-BUS-R", pc, addr, size, ret);
    aros_loop_check(addr, ret);
    aros_wait2_check(addr, ret);
    return ret;
}

static void harness_write(uint32_t addr, uint32_t value, int size)
{
    addr &= 0x00FFFFFFu;
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

    /* ROM windows are read-only */
    if (s_rom_std_size && addr >= s_rom_std_base && addr < s_rom_std_base + s_rom_std_size) return;
    if (s_rom_ext_size && addr >= s_rom_ext_base && addr < s_rom_ext_base + s_rom_ext_size) return;

    /* Chip RAM */
    if (addr < 0x200000u) {
        BellatrixMemory *mem = &bellatrix_machine_get()->memory;
        harness_watch_rw("WATCH-BUS-W", pc, addr, size, value);
        if (addr >= 0x00000800u && addr < 0x00012000u && value != 0)
            harness_watch_rw("WATCH-BPL-RAM-W", pc, addr, size, value);
        if (size == 1) bellatrix_chip_write8 (mem, addr, (uint8_t)value);
        if (size == 2) bellatrix_chip_write16(mem, addr, (uint16_t)value);
        if (size == 4) bellatrix_chip_write32(mem, addr, value);
        return;
    }

    /* Chipset / CIA / RTC */
    harness_watch_rw("WATCH-BUS-W", pc, addr, size, value);
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
    m68k_set_instr_hook_callback(harness_instr_hook);
}

void musashi_backend_reset(void)
{
    m68k_pulse_reset();
}

int musashi_backend_run(int cycles)
{
    return m68k_execute(cycles);
}
