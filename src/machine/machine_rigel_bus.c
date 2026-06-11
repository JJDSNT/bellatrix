// src/machine/machine_rigel_bus.c
//
// Address predicates, chip-RAM callbacks, machine_custom_read/write,
// machine_dispatch_read/write, bellatrix_machine_read/write.

#include "machine/machine_rigel_internal.h"

#include "machine/memory/chip_ram.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/bus/zorro3/zorro3.h"
#include "machine/bus/superbuster/superbuster.h"
#include "machine/expansion.h"

#include "debug/cpu_pc.h"
#include "host/pal.h"
#include "support.h"

#include "rigel/rigel.h"
#include "rigel/rigel_custom.h"
#include "core/rigel_context.h"   /* CIA-A sdr_full probe (keyboard log) */
#include "rigel/rigel_irq.h"

#ifdef BELLATRIX_HARNESS
#include <stdlib.h>
#endif

/* ---------------------------------------------------------------------------
 * Address predicates
 * ------------------------------------------------------------------------- */

static inline bool is_custom_addr(uint32_t addr)
{
    return (addr >= 0x00dff000u && addr <= 0x00dfffffu);
}

static inline bool is_cia_a_addr(uint32_t addr)
{
    return (addr & 1u) && (addr >= 0x00bfe001u && addr <= 0x00bfef01u);
}

static inline bool is_cia_b_addr(uint32_t addr)
{
    if (addr & 1u)
        return false;
    return (addr >= 0x00bfd000u && addr <= 0x00bfdf00u) ||
           (addr >= 0x00bfe000u && addr <= 0x00bfef00u);
}

static inline bool is_rtc_addr(uint32_t addr)
{
    return (addr >= 0x00dc0000u && addr <= 0x00dcffffu);
}

static inline bool is_autoconfig_addr(uint32_t addr)
{
    return (addr >= 0x00E80000u && addr <= 0x00E8FFFFu);
}

static inline bool is_z2_board_addr(uint32_t addr)
{
    return bellatrix_zorro2_in_board_window(addr);
}

static inline bool is_z3_board_addr(uint32_t addr)
{
    return bellatrix_zorro3_in_board_window(addr);
}

static inline bool is_superbuster_addr(uint32_t addr)
{
    return superbuster_owns(addr);
}

/* ---------------------------------------------------------------------------
 * Chip-RAM callbacks (passed to rigel_create via config.chip_ram)
 * ------------------------------------------------------------------------- */

uint16_t rigel_chip_ram_read16(void *opaque, uint32_t addr)
{
    BellatrixMachine *m = (BellatrixMachine *)opaque;
    return bellatrix_chip_read16(&m->memory, addr);
}

static void rigel_trace_chip_write(uint32_t addr, uint16_t value)
{
    static uint32_t write_count;
    static uint32_t nonzero_count;
    static int watch_init;
    static uint32_t watch_lo;
    static uint32_t watch_hi;

    addr &= 0x00ffffffu;

    if (!watch_init) {
        watch_init = 1;
#ifdef BELLATRIX_HARNESS
        const char *spec = getenv("BELLATRIX_RIGEL_CHIP_WRITE_WATCH");
        char *endptr = NULL;
        if (spec && *spec) {
            unsigned long lo = strtoul(spec, &endptr, 0);
            if (endptr && *endptr == ':') {
                unsigned long hi = strtoul(endptr + 1, &endptr, 0);
                watch_lo = (uint32_t)lo & 0x00ffffffu;
                watch_hi = (uint32_t)hi & 0x00ffffffu;
            }
        }
#endif
    }

    if (watch_lo < watch_hi && addr >= watch_lo && addr < watch_hi) {
        uint16_t dmacon = g_rigel ? rigel_custom_read16(g_rigel, 0x002u) : 0u;
        uint16_t dskpth = g_rigel ? rigel_custom_read16(g_rigel, 0x020u) : 0u;
        uint16_t dskptl = g_rigel ? rigel_custom_read16(g_rigel, 0x022u) : 0u;
        uint16_t dsklen = g_rigel ? rigel_custom_read16(g_rigel, 0x024u) : 0u;
        uint16_t bltcon0 = g_rigel ? rigel_custom_read16(g_rigel, 0x040u) : 0u;
        uint16_t bltcon1 = g_rigel ? rigel_custom_read16(g_rigel, 0x042u) : 0u;
        uint32_t bltbpt = g_rigel
            ? (((uint32_t)rigel_custom_read16(g_rigel, 0x04cu) << 16) |
               (uint32_t)rigel_custom_read16(g_rigel, 0x04eu))
            : 0u;
        uint32_t bltdpt = g_rigel
            ? (((uint32_t)rigel_custom_read16(g_rigel, 0x054u) << 16) |
               (uint32_t)rigel_custom_read16(g_rigel, 0x056u))
            : 0u;

        kprintf("[RIGEL-CHIP-WATCH-W] addr=%06x value=%04x pc=%08x "
                "dmacon=%04x dskpt=%06x dsklen=%04x "
                "bltcon=%04x/%04x bltbpt=%06x bltdpt=%06x\n",
                (unsigned)addr, (unsigned)value,
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)dmacon,
                (unsigned)((((uint32_t)dskpth & 0x001fu) << 16) |
                           ((uint32_t)dskptl & 0xfffeu)),
                (unsigned)dsklen,
                (unsigned)bltcon0,
                (unsigned)bltcon1,
                (unsigned)(bltbpt & 0x001ffffeu),
                (unsigned)(bltdpt & 0x001ffffeu));
    }

    if (!machine_rigel_rtrace_enabled())
        return;

    if (addr < 0x012000u || addr >= 0x018000u)
        return;

    if (value != 0u) {
        if (nonzero_count < 160u) {
            kprintf("[RIGEL-CHIPRAM-W] addr=%06x value=%04x pc=%08x\n",
                    (unsigned)addr, (unsigned)value,
                    (unsigned)bellatrix_debug_cpu_pc());
        }
        nonzero_count++;
        return;
    }

    if (write_count < 40u) {
        kprintf("[RIGEL-CHIPRAM-W] addr=%06x value=%04x pc=%08x\n",
                (unsigned)addr, (unsigned)value,
                (unsigned)bellatrix_debug_cpu_pc());
    }
    write_count++;
}

void rigel_chip_ram_write16(void *opaque, uint32_t addr, uint16_t value)
{
    BellatrixMachine *m = (BellatrixMachine *)opaque;
    rigel_trace_chip_write(addr, value);
    bellatrix_chip_write16(&m->memory, addr, value);
}

/* ---------------------------------------------------------------------------
 * Custom chip register read/write (DFF000 range)
 * ------------------------------------------------------------------------- */

static uint32_t machine_custom_read(uint32_t addr, unsigned int size)
{
    uint16_t word;
    uint32_t reg = addr & 0x1FEu;
    uint32_t value;
    uint32_t pc;

    if (!g_rigel)
        return 0u;

    word = rigel_custom_read16(g_rigel, reg);
    if (size == 1u)
        value = (addr & 1u) ? (uint32_t)(word & 0x00FFu)
                            : (uint32_t)(word >> 8);
    else
        value = (uint32_t)word;

    pc = bellatrix_debug_cpu_pc();
    if (machine_rigel_blitter_trace_enabled() &&
        reg == 0x002u &&
        (pc == 0x00fc5a70u || pc == 0x00fc5a78u || pc == 0x00fc5a6cu)) {
        kprintf("[RIGEL-BLT-R] pc=%08x addr=%06x size=%u word=%04x val=%08x cyc=%llu\n",
                (unsigned)pc,
                (unsigned)(addr & 0x00ffffffu),
                (unsigned)size,
                (unsigned)word,
                (unsigned)value,
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0u));
    }

    return value;
}

static void machine_custom_write(uint32_t addr, uint32_t value, unsigned int size)
{
    uint16_t word;
    uint32_t reg = addr & 0x1FEu;

    if (!g_rigel)
        return;

    if (size == 1u) {
        word = rigel_custom_read16(g_rigel, reg);
        if (addr & 1u)
            word = (uint16_t)((word & 0xFF00u) | (value & 0x00FFu));
        else
            word = (uint16_t)((word & 0x00FFu) | ((value & 0x00FFu) << 8));
    } else {
        word = (uint16_t)value;
    }

    if (machine_rigel_rtrace_enabled() &&
            (reg == 0x080u || reg == 0x082u || reg == 0x084u ||
             reg == 0x086u || reg == 0x088u || reg == 0x08au ||
             reg == 0x08eu || reg == 0x090u ||
             reg == 0x092u || reg == 0x094u ||
             (reg >= 0x0e0u && reg <= 0x0f6u) ||
             reg == 0x096u || reg == 0x100u ||
             reg == 0x108u || reg == 0x10au ||
             reg == 0x102u || reg == 0x106u ||
             (reg >= 0x180u && reg <= 0x19eu) ||
             reg == 0x1e4u)) {
        uint16_t before = rigel_custom_read16(g_rigel, reg);
        kprintf("[RIGEL-MMIO-W] reg=%03x before=%04x write=%04x size=%u pc=%08x cyc=%llu\n",
                (unsigned)reg,
                (unsigned)before,
                (unsigned)word,
                (unsigned)size,
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0u));
    }

    if (machine_rigel_blitter_trace_enabled() &&
        (reg == 0x040u || reg == 0x042u || reg == 0x044u || reg == 0x046u ||
         reg == 0x048u || reg == 0x04au || reg == 0x04cu || reg == 0x04eu ||
         reg == 0x050u || reg == 0x052u || reg == 0x054u || reg == 0x056u ||
         reg == 0x058u || reg == 0x060u || reg == 0x062u || reg == 0x064u ||
         reg == 0x066u || reg == 0x070u || reg == 0x072u || reg == 0x074u)) {
        uint16_t before = rigel_custom_read16(g_rigel, reg);
        kprintf("[RIGEL-BLT-W] pc=%08x reg=%03x before=%04x write=%04x size=%u cyc=%llu\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)reg,
                (unsigned)before,
                (unsigned)word,
                (unsigned)size,
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0u));
    }

    if (reg == 0x09au || reg == 0x09cu) {
        uint16_t before_intena = (uint16_t)rigel_custom_read16(g_rigel, 0x01cu);
        uint16_t before_intreq = (uint16_t)rigel_custom_read16(g_rigel, 0x01eu);
        uint8_t before_ipl = rigel_get_ipl(g_rigel);

        rigel_custom_write16(g_rigel, reg, word);

        if (machine_rigel_rtrace_enabled()) {
            uint16_t after_intena = (uint16_t)rigel_custom_read16(g_rigel, 0x01cu);
            uint16_t after_intreq = (uint16_t)rigel_custom_read16(g_rigel, 0x01eu);
            uint8_t after_ipl = rigel_get_ipl(g_rigel);
            kprintf("[RIGEL-INT-W] reg=%03x raw=%04x pc=%08x "
                    "intena=%04x->%04x intreq=%04x->%04x ipl=%u->%u\n",
                    (unsigned)reg, (unsigned)word,
                    (unsigned)bellatrix_debug_cpu_pc(),
                    (unsigned)before_intena, (unsigned)after_intena,
                    (unsigned)before_intreq, (unsigned)after_intreq,
                    (unsigned)before_ipl, (unsigned)after_ipl);
        }
        return;
    }

    rigel_custom_write16(g_rigel, reg, word);
}

/* ---------------------------------------------------------------------------
 * Bus dispatch
 * ------------------------------------------------------------------------- */

uint32_t machine_dispatch_read(BellatrixMachine *m, uint32_t addr, unsigned int size)
{
    uint32_t value = 0xFFFFFFFFu;

    if (is_custom_addr(addr)) {
        value = machine_custom_read(addr, size);
    } else if (is_cia_a_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0Fu);
        uint8_t sdr_pending = (reg == 0x0Cu)
            ? g_rigel->chipset.cia[0].sdr_full : 0u;
        value = rigel_cia_read(g_rigel, 0u, reg);
        if (machine_rigel_cia_trace_enabled())
            kprintf("[RIGEL-CIAA-R] pc=%08x addr=%06x reg=%u val=%02x\n",
                    (unsigned)bellatrix_debug_cpu_pc(), (unsigned)(addr & 0x00ffffffu),
                    (unsigned)reg, (unsigned)(value & 0xffu));
        if (reg == 0x0u)
            machine_rigel_trace_floppy("ciaa-pra-r", bellatrix_debug_cpu_pc(), reg, (uint8_t)value);
        /* Keyboard-path diagnostics: log SDR reads only when a keyboard
         * byte was actually pending — DiagROM polls SDR continuously and
         * unconditional logging drowns the signal. */
        if (sdr_pending)
            kprintf("[KBD] CPU read CIA-A SDR=0x%02x pc=%08x\n",
                    (unsigned)(value & 0xffu), (unsigned)bellatrix_debug_cpu_pc());
    } else if (is_cia_b_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0Fu);
        value = rigel_cia_read(g_rigel, 1u, reg);
        if (machine_rigel_cia_trace_enabled())
            kprintf("[RIGEL-CIAB-R] pc=%08x addr=%06x reg=%u val=%02x\n",
                    (unsigned)bellatrix_debug_cpu_pc(), (unsigned)(addr & 0x00ffffffu),
                    (unsigned)reg, (unsigned)(value & 0xffu));
        if (reg == 0x1u)
            machine_rigel_trace_floppy("ciab-prb-r", bellatrix_debug_cpu_pc(), reg, (uint8_t)value);
    } else if (is_rtc_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 2) & 0x0Fu);
        value = rigel_rtc_read_reg(g_rigel, reg);
    } else if (is_autoconfig_addr(addr)) {
        switch (size) {
        case 1: value = bellatrix_zorro2_config_read8(addr); break;
        case 2: value = bellatrix_zorro2_config_read16(addr); break;
        case 4: value = bellatrix_zorro2_config_read32(addr); break;
        default: break;
        }
    } else if (is_z2_board_addr(addr)) {
        if (!bellatrix_expansion_bus_read(m, addr, size, &value)) {
            switch (size) {
            case 1: value = bellatrix_zorro2_board_read8(addr); break;
            case 2: value = bellatrix_zorro2_board_read16(addr); break;
            case 4: value = bellatrix_zorro2_board_read32(addr); break;
            default: break;
            }
        }
    } else if (is_z3_board_addr(addr)) {
        switch (size) {
        case 1: value = bellatrix_zorro3_board_read8(addr); break;
        case 2: value = bellatrix_zorro3_board_read16(addr); break;
        case 4: value = bellatrix_zorro3_board_read32(addr); break;
        default: break;
        }
    } else if (is_superbuster_addr(addr)) {
        value = superbuster_read8(&m->superbuster, addr);
    } else if (bellatrix_expansion_bus_read(m, addr, size, &value)) {
        return value;
    } else {
        switch (size) {
        case 1: value = bellatrix_mem_read8(&m->memory, addr); break;
        case 2: value = bellatrix_mem_read16(&m->memory, addr); break;
        default: break;
        }
    }

    return value;
}

void machine_dispatch_write(BellatrixMachine *m, uint32_t addr, uint32_t value, unsigned int size)
{
    if (is_custom_addr(addr)) {
        machine_custom_write(addr, value, size);
    } else if (is_cia_a_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0Fu);
        if (machine_rigel_cia_trace_enabled())
            kprintf("[RIGEL-CIAA-W] pc=%08x addr=%06x reg=%u val=%02x\n",
                    (unsigned)bellatrix_debug_cpu_pc(), (unsigned)(addr & 0x00ffffffu),
                    (unsigned)reg, (unsigned)(value & 0xffu));
        rigel_cia_write(g_rigel, 0u, reg, (uint8_t)value);
        if (reg == 0x0Eu)
            machine_keyboard_on_cia_cra_write((uint8_t)value);
    } else if (is_cia_b_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0Fu);
        if (machine_rigel_cia_trace_enabled())
            kprintf("[RIGEL-CIAB-W] pc=%08x addr=%06x reg=%u val=%02x\n",
                    (unsigned)bellatrix_debug_cpu_pc(), (unsigned)(addr & 0x00ffffffu),
                    (unsigned)reg, (unsigned)(value & 0xffu));
        rigel_cia_write(g_rigel, 1u, reg, (uint8_t)value);
        if (reg == 0x1u || reg == 0x3u)
            machine_rigel_trace_floppy("ciab-w", bellatrix_debug_cpu_pc(), reg, (uint8_t)value);
    } else if (is_rtc_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 2) & 0x0Fu);
        rigel_rtc_write_reg(g_rigel, reg, (uint8_t)(value & 0x0Fu));
    } else if (is_autoconfig_addr(addr)) {
        switch (size) {
        case 1: bellatrix_zorro2_config_write8(addr, (uint8_t)value); break;
        case 2: bellatrix_zorro2_config_write16(addr, (uint16_t)value); break;
        case 4: bellatrix_zorro2_config_write32(addr, (uint32_t)value); break;
        default: break;
        }
    } else if (is_z2_board_addr(addr)) {
        if (!bellatrix_expansion_bus_write(m, addr, value, size)) {
            switch (size) {
            case 1: bellatrix_zorro2_board_write8(addr, (uint8_t)value); break;
            case 2: bellatrix_zorro2_board_write16(addr, (uint16_t)value); break;
            case 4: bellatrix_zorro2_board_write32(addr, (uint32_t)value); break;
            default: break;
            }
        }
    } else if (is_z3_board_addr(addr)) {
        switch (size) {
        case 1: bellatrix_zorro3_board_write8(addr, (uint8_t)value); break;
        case 2: bellatrix_zorro3_board_write16(addr, (uint16_t)value); break;
        case 4: bellatrix_zorro3_board_write32(addr, (uint32_t)value); break;
        default: break;
        }
    } else if (is_superbuster_addr(addr)) {
        superbuster_write8(&m->superbuster, addr, (uint8_t)value);
    } else if (bellatrix_expansion_bus_write(m, addr, value, size)) {
        return;
    } else {
        switch (size) {
        case 1: bellatrix_mem_write8(&m->memory, addr, (uint8_t)value); break;
        case 2: bellatrix_mem_write16(&m->memory, addr, (uint16_t)value); break;
        case 4: bellatrix_mem_write32(&m->memory, addr, (uint32_t)value); break;
        default: break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public bus entry points
 * ------------------------------------------------------------------------- */

uint32_t bellatrix_machine_read(uint32_t addr, unsigned int size)
{
    BellatrixMachine *m = &g_machine;

    /* One bus cycle elapsed; flush partial quantum so register reads
     * (VPOS, INTREQ, DMA owner) reflect the current Rigel state. */
    {
        uint64_t scaled = (uint64_t)s_cpu_cck_rem + 1u;
        s_cpu_approx += scaled / 2u;
        s_cpu_cck_rem = (uint32_t)(scaled & 1u);
    }
    machine_flush_for_bus(m);

    if (size == 4u) {
        uint32_t hi = machine_dispatch_read(m, addr, 2u);
        uint32_t lo = machine_dispatch_read(m, addr + 2u, 2u);
        return (hi << 16) | lo;
    }

    return machine_dispatch_read(m, addr, size);
}

void bellatrix_machine_write(uint32_t addr, uint32_t value, unsigned int size)
{
    BellatrixMachine *m = &g_machine;

    /* One bus cycle elapsed; flush so Rigel sees the write at the right
     * time and the post-write IPL reflects the updated register. */
    {
        uint64_t scaled = (uint64_t)s_cpu_cck_rem + 1u;
        s_cpu_approx += scaled / 2u;
        s_cpu_cck_rem = (uint32_t)(scaled & 1u);
    }
    machine_flush_for_bus(m);

    if (size == 4u) {
        machine_dispatch_write(m, addr, value >> 16, 2u);
        machine_dispatch_write(m, addr + 2u, value & 0xFFFFu, 2u);
    } else {
        machine_dispatch_write(m, addr, value, size);
    }

    bellatrix_machine_sync_ipl();
}
