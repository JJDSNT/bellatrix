// tools/harness/musashi_backend.c
//
// Musashi CpuBackend — implements the two machine callbacks using Musashi,
// and provides the memory read/write callbacks that Musashi requires.
//
// Address map served by this dispatcher:
//   0x000000–BELLATRIX_CHIP_RAM_END  chip RAM (or ROM overlay at boot)
//   0xE00000–0xEFFFFF  extended ROM (1 MB ROMs only — first 512 KB)
//   0xF80000–0xFFFFFF  standard ROM (Kickstart / AROS second 512 KB)
//   everything else    delegated to bellatrix_machine_read/write (chipset/CIA/RTC)
//
// ROM size → layout:
//   256 KB → single window at 0xFC0000
//   512 KB → single window at 0xF80000
//   1 MB   → first 512 KB at 0xE00000, second 512 KB at 0xF80000

#include "musashi_backend.h"
#include "bridge/bellatrix_bridge.h"
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
static uint32_t s_boot_trace_until_pc = 0;
static uint32_t s_boot_trace_ack_count = 0;
static uint32_t s_boot_display_a5 = 0;
static uint32_t s_boot_display_ctx = 0;
static uint32_t s_boot_display_aux = 0;
static uint32_t s_boot_display_buf0 = 0;
static uint32_t s_boot_display_buf1 = 0;

#define BOOT_DISPLAY_BUF_SIZE 0x00001F40u

static uint32_t harness_chip_read(uint32_t addr, int size);

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
    addr = bellatrix_bridge_normalize_addr(addr);

    if (addr >= 0x0000A4C0u && addr <= 0x0000A580u)
        return 1;
    if (addr >= 0x0000A572u && addr <= 0x0000A8C0u)
        return 1;
    if (addr >= 0x0000C4B2u && addr <= 0x0000C800u)
        return 1;

    return 0;
}

static int harness_watch_boot_pc(uint32_t pc)
{
    if (pc >= 0x00FC0718u && pc <= 0x00FC0788u)
        return 1;
    if (pc >= 0x00FE8530u && pc <= 0x00FE867Cu)
        return 1;
    if (pc >= 0x00FE960Cu && pc <= 0x00FE96FCu)
        return 1;
    if (pc >= 0x00FEAA5Cu && pc <= 0x00FEAAC2u)
        return 1;
    return 0;
}

static int harness_watch_boot_addr(uint32_t addr)
{
    addr = bellatrix_bridge_normalize_addr(addr);

    if (addr >= 0x000018B0u && addr <= 0x00001920u)
        return 1;

    switch (addr)
    {
        case 0x00BFE001u: /* CIAA PRA */
        case 0x00BFD100u: /* CIAB PRB */
        case 0x00DFF024u: /* DSKLEN */
        case 0x00DFF07Eu: /* DSKPTH */
        case 0x00DFF080u: /* DSKPTL */
        case 0x00DFF07Cu: /* DSKSYNC */
        case 0x00DFF09Au: /* INTENA */
        case 0x00DFF09Cu: /* INTREQ */
        case 0x00DFF09Eu: /* ADKCON */
        case 0x00DFF096u: /* DMACON */
            return 1;
        default:
            return 0;
    }
}

static int harness_watch_boot_payload_addr(uint32_t addr)
{
    addr = bellatrix_bridge_normalize_addr(addr);

    if (addr >= 0x0000A000u && addr <= 0x0000AFFFu)
        return 1;
    if (addr >= 0x0000C000u && addr <= 0x0000CFFFu)
        return 1;

    return 0;
}

static void harness_update_boot_display_block(uint32_t pc)
{
    uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
    uint32_t ctx = 0;
    uint32_t aux = 0;
    uint32_t buf0 = 0;
    uint32_t buf1 = 0;

    if (pc < 0x00FE8768u || pc > 0x00FE8960u)
        return;
    if (!bellatrix_chip_addr_contains_range(a5, 0x2Cu))
        return;

    ctx = harness_chip_read(a5 + 0x1Cu, 4);
    aux = harness_chip_read(a5 + 0x20u, 4);
    buf0 = harness_chip_read(a5 + 0x24u, 4);
    buf1 = harness_chip_read(a5 + 0x28u, 4);

    if (a5 == s_boot_display_a5 &&
        ctx == s_boot_display_ctx &&
        aux == s_boot_display_aux &&
        buf0 == s_boot_display_buf0 &&
        buf1 == s_boot_display_buf1)
        return;

    s_boot_display_a5 = a5;
    s_boot_display_ctx = ctx;
    s_boot_display_aux = aux;
    s_boot_display_buf0 = buf0;
    s_boot_display_buf1 = buf1;

    printf("[BOOT-DISPLAY-BLOCK] pc=%08x A5=%06x A5+1c=%08x A5+20=%08x "
           "A5+24=%08x A5+28=%08x\n",
           (unsigned)pc,
           (unsigned)a5,
           (unsigned)ctx,
           (unsigned)aux,
           (unsigned)buf0,
           (unsigned)buf1);
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

    if (harness_watch_boot_pc(pc) && harness_watch_boot_addr(addr))
    {
        uint32_t a4 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu;
        uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
        uint32_t a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
        uint32_t a5_04 = 0;
        uint32_t a5_4c = 0;
        uint32_t a4_00 = 0;
        uint32_t a4_04 = 0;
        uint32_t vec_1c8 = 0;
        uint16_t vec_1c4 = 0;

        if (bellatrix_chip_addr_contains_range(a5, 0x50u))
        {
            a5_04 = harness_chip_read(a5 + 0x04u, 4);
            a5_4c = harness_chip_read(a5 + 0x4Cu, 4);
        }

        if (bellatrix_chip_addr_contains_range(a4, 0x08u))
        {
            a4_00 = harness_chip_read(a4 + 0x00u, 4);
            a4_04 = harness_chip_read(a4 + 0x04u, 4);
        }

        if (a6 >= 0x000001C8u &&
            bellatrix_chip_addr_contains_range(a6 - 0x000001C8u, 6u))
        {
            uint32_t vaddr = a6 - 0x000001C8u;
            vec_1c8 = harness_chip_read(vaddr + 0x00u, 4);
            vec_1c4 = (uint16_t)harness_chip_read(vaddr + 0x04u, 2);
        }

        printf("[BOOT-WATCH] tag=%s pc=%08x addr=%08x size=%d val=%08x "
               "D0=%08x D2=%08x A4=%06x A4+00=%08x A4+04=%08x "
               "A5=%06x A5+04=%08x A5+4c=%08x "
               "A6=%06x V-1c8=%08x:%04x\n",
               tag,
               (unsigned)pc,
               (unsigned)(bellatrix_bridge_normalize_addr(addr) & 0x00FFFFFFu),
               size,
               (unsigned)value,
               (unsigned)d0,
               (unsigned)d2,
               (unsigned)a4,
               (unsigned)a4_00,
               (unsigned)a4_04,
               (unsigned)a5,
               (unsigned)a5_04,
               (unsigned)a5_4c,
               (unsigned)a6,
               (unsigned)vec_1c8,
               (unsigned)vec_1c4);
    }

    if (!harness_watch_gfx_pc(pc) && !harness_watch_gfx_addr(addr))
        return;

    printf("[%s] pc=%08x addr=%08x size=%d val=%08x\n",
           tag,
           (unsigned)pc,
           (unsigned)(addr & 0x00FFFFFFu),
           size,
           (unsigned)value);
}

static int harness_is_boot_bitplane_payload_addr(uint32_t addr)
{
    addr &= 0x00FFFFFFu;

    if (addr >= 0x0000A000u && addr < 0x0000B000u)
        return 1;
    if (addr >= 0x0000C000u && addr < 0x0000D000u)
        return 1;

    return 0;
}

static void harness_watch_boot_bitplane_write(uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    const char *target = "static";
    uint32_t addr24 = addr & 0x00FFFFFFu;

    if (!harness_is_boot_bitplane_payload_addr(addr24) || value == 0)
        return;

    if (addr24 >= s_boot_display_buf0 &&
        addr24 < s_boot_display_buf0 + BOOT_DISPLAY_BUF_SIZE)
        target = "BUF0";
    else if (addr24 >= s_boot_display_buf1 &&
             addr24 < s_boot_display_buf1 + BOOT_DISPLAY_BUF_SIZE)
        target = "BUF1";

    printf("[BOOT-DISPLAY-BLOCK-W] pc=%08x target=%s addr=%06x size=%d val=%08x\n",
           (unsigned)pc,
           target,
           (unsigned)addr24,
           size,
           (unsigned)value);
}

static void harness_watch_boot_dynamic_buffer_write(uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    uint32_t addr24 = addr & 0x00FFFFFFu;
    const char *target = NULL;

    if (value == 0)
        return;

    if (s_boot_display_buf0 != 0 &&
        addr24 >= s_boot_display_buf0 &&
        addr24 < s_boot_display_buf0 + BOOT_DISPLAY_BUF_SIZE)
        target = "BUF0";
    else if (s_boot_display_buf1 != 0 &&
             addr24 >= s_boot_display_buf1 &&
             addr24 < s_boot_display_buf1 + BOOT_DISPLAY_BUF_SIZE)
        target = "BUF1";

    if (!target)
        return;

    printf("[BOOT-DISPLAY-BUFFER-W] pc=%08x target=%s addr=%06x size=%d val=%08x\n",
           (unsigned)pc,
           target,
           (unsigned)addr24,
           size,
           (unsigned)value);
}

static void harness_watch_dskblk_ack(uint32_t pc, uint32_t addr, int size, uint32_t value)
{
    BellatrixMachine *m = bellatrix_machine_get();
    uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
    uint32_t a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
    uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
    uint32_t d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
    uint32_t d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
    uint32_t bpl1 = ((((uint32_t)(m->agnus.bplpth[0] & 0x001Fu)) << 16) |
                     ((uint32_t)m->agnus.bplptl[0] & 0xFFFEu));
    uint32_t bpl2 = ((((uint32_t)(m->agnus.bplpth[1] & 0x001Fu)) << 16) |
                     ((uint32_t)m->agnus.bplptl[1] & 0xFFFEu));
    uint32_t cop1 = m->agnus.copper.cop1lc & 0x001FFFFEu;
    uint32_t cop2 = m->agnus.copper.cop2lc & 0x001FFFFEu;
    uint32_t p0 = harness_chip_read(0x0000A572u, 4);
    uint32_t p1 = harness_chip_read(0x0000C4B2u, 4);

    if ((addr & 0x00FFFFFFu) != 0x00DFF09Cu)
        return;
    if (size != 2)
        return;
    if ((value & 0xFFFFu) != 0x0002u &&
        (value & 0xFFFFu) != 0x1002u)
        return;

    s_boot_trace_ack_count++;
    s_boot_trace_until_pc = pc + 0x00002000u;

    printf("[BOOT-DSKBLK-ACK] pc=%08x raw=%04x D0=%08x D1=%08x D2=%08x "
           "A5=%06x A6=%06x intena=%04x intreq=%04x dmacon=%04x bplcon0=%04x "
           "bpl1=%05x bpl2=%05x cop1=%05x cop2=%05x p0=%08x p1=%08x\n",
           (unsigned)pc,
           (unsigned)(value & 0xFFFFu),
           (unsigned)d0,
           (unsigned)d1,
           (unsigned)d2,
           (unsigned)a5,
           (unsigned)a6,
           (unsigned)m->paula.irq.intena,
           (unsigned)m->paula.irq.intreq,
           (unsigned)m->agnus.dmacon,
           (unsigned)m->agnus.bplcon0,
           (unsigned)bpl1,
           (unsigned)bpl2,
           (unsigned)cop1,
           (unsigned)cop2,
           (unsigned)p0,
           (unsigned)p1);
}

static void harness_trace_post_dskblk_window(uint32_t pc)
{
    BellatrixMachine *m = bellatrix_machine_get();
    uint32_t p0 = harness_chip_read(0x0000A572u, 4);
    uint32_t p1 = harness_chip_read(0x0000C4B2u, 4);

    if (s_boot_trace_until_pc == 0)
        return;
    if (pc < 0x00FC0000u || pc > s_boot_trace_until_pc)
        return;

    switch (pc)
    {
    case 0x00FC4AF0u:
    case 0x00FC5A6Cu:
    case 0x00FC5A78u:
    case 0x00FE881Au:
    case 0x00FE8820u:
    case 0x00FE8844u:
    case 0x00FCCBA2u:
    case 0x00FCCBB8u:
    case 0x00FCCBBEu:
    case 0x00FCCBC6u:
    case 0x00FCCBD2u:
    case 0x00FCCBF2u:
        break;
    default:
        return;
    }

    printf("[BOOT-AFTER-DSK] ack=%u pc=%08x dmacon=%04x bplcon0=%04x "
           "bpl1=%05x bpl2=%05x cop1=%05x cop2=%05x p0=%08x p1=%08x\n",
           (unsigned)s_boot_trace_ack_count,
           (unsigned)pc,
           (unsigned)m->agnus.dmacon,
           (unsigned)m->agnus.bplcon0,
           (unsigned)((((uint32_t)(m->agnus.bplpth[0] & 0x001Fu)) << 16) |
                      ((uint32_t)m->agnus.bplptl[0] & 0xFFFEu)),
           (unsigned)((((uint32_t)(m->agnus.bplpth[1] & 0x001Fu)) << 16) |
                      ((uint32_t)m->agnus.bplptl[1] & 0xFFFEu)),
           (unsigned)(m->agnus.copper.cop1lc & 0x001FFFFEu),
           (unsigned)(m->agnus.copper.cop2lc & 0x001FFFFEu),
           (unsigned)p0,
           (unsigned)p1);
}

static void harness_probe_display_setup(uint32_t pc)
{
    uint32_t a5;
    uint32_t a5_08;
    uint32_t a5_0c;
    uint32_t a5_10;
    uint32_t a5_14;
    uint32_t a5_18;
    uint32_t a5_1c;
    uint32_t a5_20;
    uint32_t a5_24;
    uint32_t a5_28;

    if (pc < 0x00FE8768u || pc > 0x00FE8960u)
        return;

    a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
    if (!bellatrix_chip_addr_contains_range(a5, 0x2Cu))
        return;

    a5_08 = harness_chip_read(a5 + 0x08u, 4);
    a5_0c = harness_chip_read(a5 + 0x0Cu, 4);
    a5_10 = harness_chip_read(a5 + 0x10u, 4);
    a5_14 = harness_chip_read(a5 + 0x14u, 4);
    a5_18 = harness_chip_read(a5 + 0x18u, 4);
    a5_1c = harness_chip_read(a5 + 0x1Cu, 4);
    a5_20 = harness_chip_read(a5 + 0x20u, 4);
    a5_24 = harness_chip_read(a5 + 0x24u, 4);
    a5_28 = harness_chip_read(a5 + 0x28u, 4);

    printf("[BOOT-DISPLAY-SETUP] pc=%08x A5=%06x "
           "+08=%08x +0c=%08x +10=%08x +14=%08x +18=%08x "
           "+1c=%08x +20=%08x +24=%08x +28=%08x\n",
           (unsigned)pc,
           (unsigned)a5,
           (unsigned)a5_08,
           (unsigned)a5_0c,
           (unsigned)a5_10,
           (unsigned)a5_14,
           (unsigned)a5_18,
           (unsigned)a5_1c,
           (unsigned)a5_20,
           (unsigned)a5_24,
           (unsigned)a5_28);
}

static void harness_probe_display_writer(uint32_t pc)
{
    uint32_t a0;
    uint32_t a1;
    uint32_t a4;
    uint32_t a5;
    uint32_t a6;
    uint32_t d0;
    uint32_t d1;
    uint32_t d2;
    uint32_t d3;
    uint32_t a5_24 = 0;
    uint32_t a5_28 = 0;
    uint32_t sample0 = 0;
    uint32_t sample1 = 0;

    if (pc != 0x00FE9AAAu)
        return;

    a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;
    a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
    a4 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu;
    a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
    a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
    d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
    d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
    d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
    d3 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D3);

    if (bellatrix_chip_addr_contains_range(a5, 0x2Cu))
    {
        a5_24 = harness_chip_read(a5 + 0x24u, 4);
        a5_28 = harness_chip_read(a5 + 0x28u, 4);
    }

    if (bellatrix_chip_addr_contains_range(a0, 8u))
    {
        sample0 = harness_chip_read(a0 + 0x00u, 4);
        sample1 = harness_chip_read(a0 + 0x04u, 4);
    }

    printf("[BOOT-DISPLAY-WRITER] pc=%08x D0=%08x D1=%08x D2=%08x D3=%08x "
           "A0=%06x A1=%06x A4=%06x A5=%06x A6=%06x "
           "A5+24=%08x A5+28=%08x A0[0]=%08x A0[4]=%08x\n",
           (unsigned)pc,
           (unsigned)d0,
           (unsigned)d1,
           (unsigned)d2,
           (unsigned)d3,
           (unsigned)a0,
           (unsigned)a1,
           (unsigned)a4,
           (unsigned)a5,
           (unsigned)a6,
           (unsigned)a5_24,
           (unsigned)a5_28,
           (unsigned)sample0,
           (unsigned)sample1);
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
        harness_dump_disasm("HH-BUILD", pc);
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

static void harness_probe_gfx_builder(void)
{
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

    switch (pc)
    {
        case 0x00FE881Au:
        case 0x00FE8820u:
        case 0x00FE8844u:
        case 0x00FCCBA2u:
        case 0x00FCCBB8u:
        case 0x00FCCBBEu:
        case 0x00FCCBC6u:
        case 0x00FCCBD2u:
        case 0x00FCCBF2u:
        case 0x00FC5A6Cu:
        case 0x00FC5A78u:
            break;
        default:
            return;
    }

    {
        uint32_t a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;
        uint32_t a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
        uint32_t a2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu;
        uint32_t a3 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A3) & 0x00FFFFFFu;
        uint32_t a4 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu;
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
        uint32_t d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
        uint32_t d3 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D3);
        uint32_t a542 = harness_chip_read(0x0000A542u, 4);
        uint32_t a552 = harness_chip_read(0x0000A552u, 4);
        uint32_t a556 = harness_chip_read(0x0000A556u, 4);
        uint32_t p0 = harness_chip_read(0x0000A572u, 4);
        uint32_t p1 = harness_chip_read(0x0000C4B2u, 4);

        harness_dump_disasm("GFX-BUILD", pc);
        printf("[GFX-BUILD] pc=%08x D0=%08x D1=%08x D2=%08x D3=%08x "
               "A0=%06x A1=%06x A2=%06x A3=%06x A4=%06x "
               "a542=%08x a552=%08x a556=%08x "
               "p0@a572=%08x p1@c4b2=%08x\n",
               (unsigned)pc,
               (unsigned)d0,
               (unsigned)d1,
               (unsigned)d2,
               (unsigned)d3,
               (unsigned)a0,
               (unsigned)a1,
               (unsigned)a2,
               (unsigned)a3,
               (unsigned)a4,
               (unsigned)a542,
               (unsigned)a552,
               (unsigned)a556,
               (unsigned)p0,
               (unsigned)p1);
    }
}

static void harness_probe_gfx_calls(uint32_t pc)
{
    switch (pc)
    {
        case 0x00FE8824u:
        case 0x00FE8836u:
        case 0x00FC5E2Cu:
        case 0x00FC63C6u:
            break;
        default:
            return;
    }

    {
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
        uint32_t a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;
        uint32_t a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
        uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
        uint32_t a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
        uint32_t a5_10 = bellatrix_chip_addr_contains_range(a5 + 0x10u, 0x1cu)
                             ? harness_chip_read(a5 + 0x10u, 4)
                             : 0;
        uint32_t a5_14 = bellatrix_chip_addr_contains_range(a5 + 0x14u, 0x18u)
                             ? harness_chip_read(a5 + 0x14u, 4)
                             : 0;
        uint32_t a5_1c = bellatrix_chip_addr_contains_range(a5 + 0x1cu, 0x10u)
                             ? harness_chip_read(a5 + 0x1cu, 4)
                             : 0;
        uint32_t a5_24 = bellatrix_chip_addr_contains_range(a5 + 0x24u, 0x08u)
                             ? harness_chip_read(a5 + 0x24u, 4)
                             : 0;
        uint32_t a5_28 = bellatrix_chip_addr_contains_range(a5 + 0x28u, 0x04u)
                             ? harness_chip_read(a5 + 0x28u, 4)
                             : 0;

        harness_dump_disasm("GFX-CALL", pc);
        printf("[GFX-CALL] pc=%08x D0=%08x D1=%08x A0=%06x A1=%06x A5=%06x A6=%06x "
               "A5+10=%08x A5+14=%08x A5+1c=%08x A5+24=%08x A5+28=%08x\n",
               (unsigned)pc,
               (unsigned)d0,
               (unsigned)d1,
               (unsigned)a0,
               (unsigned)a1,
               (unsigned)a5,
               (unsigned)a6,
               (unsigned)a5_10,
               (unsigned)a5_14,
               (unsigned)a5_1c,
               (unsigned)a5_24,
               (unsigned)a5_28);
    }
}

static void harness_probe_boot_path(uint32_t pc)
{
    switch (pc)
    {
        case 0x00FE8560u:
        case 0x00FE8574u:
        case 0x00FE85A0u:
        case 0x00FE85AAu:
        case 0x00FE85C0u:
        case 0x00FE85CCu:
        case 0x00FE8600u:
        case 0x00FE8610u:
        case 0x00FE8616u:
        case 0x00FE861Au:
            break;
        default:
            return;
    }

    {
        uint32_t a5 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A5) & 0x00FFFFFFu;
        uint32_t a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
        uint32_t a2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu;
        uint32_t a4 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A4) & 0x00FFFFFFu;
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D2);
        uint32_t a5_04 = 0;
        uint32_t a5_4c = 0;
        uint16_t a1_1c = 0;

        if (bellatrix_chip_addr_contains_range(a5, 0x50u))
        {
            a5_04 = harness_chip_read(a5 + 0x04u, 4);
            a5_4c = harness_chip_read(a5 + 0x4Cu, 4);
        }

        if (bellatrix_chip_addr_contains_range(a1, 0x1Eu))
            a1_1c = (uint16_t)harness_chip_read(a1 + 0x1Cu, 2);

        printf("[BOOT-PATH] pc=%08x D0=%08x D2=%08x A1=%06x A2=%06x A4=%06x A5=%06x "
               "A5+04=%08x A5+4c=%08x A1+1c=%04x\n",
               (unsigned)pc,
               (unsigned)d0,
               (unsigned)d2,
               (unsigned)a1,
               (unsigned)a2,
               (unsigned)a4,
               (unsigned)a5,
               (unsigned)a5_04,
               (unsigned)a5_4c,
               (unsigned)a1_1c);
    }
}

static void harness_probe_exec_io(uint32_t pc)
{
    switch (pc)
    {
        case 0x00FC0718u:
        case 0x00FC0726u:
        case 0x00FC072Eu:
        case 0x00FC0746u:
        case 0x00FC075Au:
        case 0x00FC0760u:
        case 0x00FC0780u:
            break;
        default:
            return;
    }

    {
        uint32_t a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;
        uint32_t a1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A1) & 0x00FFFFFFu;
        uint32_t a2 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A2) & 0x00FFFFFFu;
        uint32_t a6 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A6) & 0x00FFFFFFu;
        uint32_t d0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D0);
        uint32_t d1 = (uint32_t)m68k_get_reg(NULL, M68K_REG_D1);
        uint32_t req_ln_succ = 0;
        uint32_t req_ln_pred = 0;
        uint32_t req_dev = 0;
        uint32_t req_unit = 0;
        uint16_t req_cmd = 0;
        uint8_t req_flags = 0;
        int8_t req_err = 0;
        uint32_t unit_msgport = 0;
        uint8_t unit_num = 0;

        if (bellatrix_chip_addr_contains_range(a1, 0x2Du))
        {
            req_ln_succ = harness_chip_read(a1 + 0x00u, 4);
            req_ln_pred = harness_chip_read(a1 + 0x04u, 4);
            req_dev = harness_chip_read(a1 + 0x10u, 4);
            req_unit = harness_chip_read(a1 + 0x14u, 4);
            req_cmd = (uint16_t)harness_chip_read(a1 + 0x1Cu, 2);
            req_flags = (uint8_t)harness_chip_read(a1 + 0x1Eu, 1);
            req_err = (int8_t)harness_chip_read(a1 + 0x1Fu, 1);
        }

        if (bellatrix_chip_addr_contains_range(req_unit, 0x10u))
        {
            unit_msgport = harness_chip_read(req_unit + 0x00u, 4);
            unit_num = (uint8_t)harness_chip_read(req_unit + 0x09u, 1);
        }

        printf("[EXEC-IO] pc=%08x D0=%08x D1=%08x A0=%06x A1=%06x A2=%06x A6=%06x "
               "ln=%08x/%08x dev=%06x unit=%06x cmd=%04x flags=%02x err=%d "
               "unit_mp=%06x unit_num=%u\n",
               (unsigned)pc,
               (unsigned)d0,
               (unsigned)d1,
               (unsigned)a0,
               (unsigned)a1,
               (unsigned)a2,
               (unsigned)a6,
               (unsigned)req_ln_succ,
               (unsigned)req_ln_pred,
               (unsigned)req_dev,
               (unsigned)req_unit,
               (unsigned)req_cmd,
               (unsigned)req_flags,
               (int)req_err,
               (unsigned)unit_msgport,
               (unsigned)unit_num);
    }
}

static void harness_probe_vbl_dispatch_node(uint32_t pc)
{
    static int s_done = 0;
    if (pc != 0x00FC182Au)
        return;

    uint32_t a0 = (uint32_t)m68k_get_reg(NULL, M68K_REG_A0) & 0x00FFFFFFu;

    /* Only fire when A0 is in the callback node area we care about */
    if (a0 < 0x1880u || a0 > 0x1900u)
        return;
    if (s_done)
        return;
    s_done = 1;

    printf("[VBL-DISP-NODE] pc=%08x A0=%08x\n", (unsigned)pc, (unsigned)a0);

    /* Dump 64 bytes starting 16 bytes before A0 */
    uint32_t base = (a0 >= 0x10u) ? (a0 - 0x10u) : 0u;
    uint32_t i;
    for (i = 0; i < 64u; i += 4u) {
        uint32_t addr = base + i;
        uint32_t v = harness_chip_read(addr, 4);
        printf("[VBL-DISP-NODE]   %06x: %08x\n", (unsigned)addr, (unsigned)v);
    }
}

static void harness_instr_hook(unsigned int pc)
{
    harness_update_boot_display_block((uint32_t)pc);
    harness_probe_display_setup((uint32_t)pc);
    harness_probe_display_writer((uint32_t)pc);
    harness_probe_happy_builder();
    harness_probe_gfx_builder();
    harness_probe_gfx_calls((uint32_t)pc);
    harness_probe_boot_path((uint32_t)pc);
    harness_probe_vbl_dispatch_node((uint32_t)pc);
    harness_probe_exec_io((uint32_t)pc);
    harness_trace_post_dskblk_window((uint32_t)pc);
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
    if (bellatrix_chip_addr_contains(addr)) {
        /* Overlay: reads from low page redirect to standard ROM */
        if (harness_overlay() && addr < BELLATRIX_CHIP_BOOT_SIZE && s_rom_std_size) {
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
    ret = bellatrix_bridge_cpu_read(addr, (unsigned int)size);
    harness_watch_rw("WATCH-BUS-R", pc, addr, size, ret);
    aros_loop_check(addr, ret);
    aros_wait2_check(addr, ret);
    return ret;
}

static void harness_write(uint32_t addr, uint32_t value, int size)
{
    addr = bellatrix_bridge_normalize_addr(addr);
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);

    /* ROM windows are read-only */
    if (s_rom_std_size && addr >= s_rom_std_base && addr < s_rom_std_base + s_rom_std_size) return;
    if (s_rom_ext_size && addr >= s_rom_ext_base && addr < s_rom_ext_base + s_rom_ext_size) return;

    /* Chip RAM */
    if (bellatrix_chip_addr_contains(addr)) {
        BellatrixMemory *mem = &bellatrix_machine_get()->memory;
        harness_watch_rw("WATCH-BUS-W", pc, addr, size, value);
        harness_watch_boot_bitplane_write(pc, addr, size, value);
        harness_watch_boot_dynamic_buffer_write(pc, addr, size, value);
        if (harness_watch_boot_payload_addr(addr) && value != 0)
            harness_watch_rw("WATCH-BOOT-PAYLOAD-W", pc, addr, size, value);
        if (addr >= 0x00000800u && addr < 0x00012000u && value != 0)
            harness_watch_rw("WATCH-BPL-RAM-W", pc, addr, size, value);
        if (size == 1) bellatrix_chip_write8 (mem, addr, (uint8_t)value);
        if (size == 2) bellatrix_chip_write16(mem, addr, (uint16_t)value);
        if (size == 4) bellatrix_chip_write32(mem, addr, value);
        /* Track writes that may touch bytes 0x1892..0x1895 */
        uint32_t wend = addr + (uint32_t)size - 1u;
        if (wend >= 0x1892u && addr <= 0x1895u) {
            uint32_t b92 = bellatrix_chip_read32(mem, 0x1892u);
            printf("[1892-WATCH] pc=%08x write addr=%06x size=%d val=%08x → [1892]=%08x\n",
                   (unsigned)pc, (unsigned)addr, size, (unsigned)value, (unsigned)b92);
        }
        return;
    }

    /* Chipset / CIA / RTC */
    harness_watch_rw("WATCH-BUS-W", pc, addr, size, value);
    bellatrix_bridge_cpu_write(addr, value, (unsigned int)size);
    harness_watch_dskblk_ack(pc, addr, size, value);
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
