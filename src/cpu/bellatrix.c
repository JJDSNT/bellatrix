// src/cpu/bellatrix.c
//
// Emu68 integration entry point for the Bellatrix chipset emulator.
// Routes every unmapped M68K bus access to the appropriate chipset module.

#include "bellatrix.h"
#include "core/machine.h"
#include "core/btrace.h"
#include "chipset/agnus/agnus.h"
#include "chipset/cia/cia.h"
#include "chipset/denise/denise.h"
#include "host/pal.h"
#include "mmu.h"
#include "A64.h"
#include "support.h"

/* extern'd by start.c ROM loading code */
uint32_t rom_mapped = 0;

/* ---------------------------------------------------------------------------
 * Wall-clock driven machine step (strong override of pal_core.c weak stub).
 * Called by PAL_Runtime_Poll() on every bus access.
 * ------------------------------------------------------------------------- */

#define BELLATRIX_M68K_CLOCK_HZ 7093790ULL

void bellatrix_runtime_host_step(uint64_t host_now, uint64_t host_freq)
{
    static uint64_t s_last = 0;

    if (!s_last) { s_last = host_now; return; }

    uint64_t delta = host_now - s_last;
    s_last = host_now;

    if (host_freq && delta > host_freq) delta = host_freq;
    if (!host_freq) return;

    uint64_t m68k_cycles = (delta * BELLATRIX_M68K_CLOCK_HZ) / host_freq;
    if (m68k_cycles)
        bellatrix_machine_advance((uint32_t)m68k_cycles);
}

/* ---------------------------------------------------------------------------
 * Overlay state (CIA-A PRA bit 0 — OVL)
 * ------------------------------------------------------------------------- */

static int s_overlay = 1;

#define BTRACE_CONTROL_ADDR  0xDFFF00u

static inline int cia_reg(uint32_t addr)
{
    return (int)((addr >> 8) & 0xF);
}

/* ---------------------------------------------------------------------------
 * IPL sync — call after any CIA/chipset register access
 * ------------------------------------------------------------------------- */

static void update_ipl(void)
{
    bellatrix_machine_sync_ipl();
}

/* ---------------------------------------------------------------------------
 * Overlay switch
 * ------------------------------------------------------------------------- */

static void set_overlay(int new_overlay)
{
    if (new_overlay == s_overlay) return;
    s_overlay = new_overlay;
    if (s_overlay)
        mmu_map(0xf80000, 0x000000, 4096,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    else
        mmu_map(0x000000, 0x000000, 4096,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
}

/* ---------------------------------------------------------------------------
 * ROM physical base in Emu68 kernel virtual space
 * ------------------------------------------------------------------------- */

#define ROM_KVIRT  0xffffff9000f80000ULL

static inline uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

/* ---------------------------------------------------------------------------
 * Chip RAM direct write (bypasses MMU read-only overlay mapping).
 * OVL only affects reads — writes to 0x000000-0x1FFFFF always go to chip RAM.
 * ------------------------------------------------------------------------- */

#define CHIP_RAM_VIRT  0xffffff9000000000ULL
#define CHIP_RAM_SIZE  0x200000UL

static void chip_ram_write(uint32_t addr, uint32_t value, int size)
{
    uintptr_t virt = CHIP_RAM_VIRT + addr;
    switch (size) {
    case 1: *(volatile uint8_t  *)virt = (uint8_t) value; break;
    case 2: *(volatile uint16_t *)virt = (uint16_t)value; break;
    case 4: *(volatile uint32_t *)virt = value;           break;
    }
}

/* ---------------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------------- */

void bellatrix_init(void)
{
    extern struct M68KState *__m68k_state;

    PAL_Debug_Init(115200);
    btrace_init();

    bellatrix_machine_init(__m68k_state);

    /* Bellatrix-specific CIA-A defaults: OVL and LED are outputs */
    BellatrixMachine *m = bellatrix_machine_get();
    m->cia_a.ddra = 0x03;

    /* ROM diagnostic */
    {
        const uint8_t *rom = (const uint8_t *)ROM_KVIRT;
        kprintf("[BELA] rom_mapped=%d\n", (int)rom_mapped);
        if (rom_mapped) {
            uint32_t isp = read_be32(rom);
            uint32_t pc  = read_be32(rom + 4);
            kprintf("[BELA] ROM @ 0xf80000: %02x %02x %02x %02x  %02x %02x %02x %02x\n",
                    rom[0], rom[1], rom[2], rom[3],
                    rom[4], rom[5], rom[6], rom[7]);
            kprintf("[BELA] Reset vectors: ISP=0x%08x  PC=0x%08x\n", isp, pc);
            if (pc < 0x00f80000 || pc > 0x00ffffff)
                kprintf("[BELA] WARNING: PC 0x%08x outside ROM range -- ROM may be corrupt!\n", pc);
        } else {
            kprintf("[BELA] WARNING: rom_mapped=0 -- M68K will start at PC=0.\n");
        }
    }

    /* Chip RAM: 2 MB R/W */
    mmu_map(0x000000, 0x000000, 0x200000,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    /* ROM overlay at address 0 (overlay=1): reset vectors visible at 0x000000 */
    mmu_map(0xf80000, 0x000000, 4096,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    s_overlay = 1;

    /* Chipset trap mappings — AF=0 pages so every CIA/custom access faults */
    mmu_map(0x200000, 0x200000, 0xA00000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    mmu_map(0xC00000, 0xC00000, 0x200000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    mmu_map(0xF00000, 0xF00000, 0x80000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);

    /* Overlay sanity-check */
    if (rom_mapped) {
        uint32_t word0;
        asm volatile("mov x9, #0\n\t"
                     "ldr %w0, [x9]\n"
                     : "=r"(word0) : : "x9", "memory");
        kprintf("[BELA] Overlay check virt[0:3]: %02x %02x %02x %02x  "
                "(expect same as ROM bytes above)\n",
                (word0 >> 24) & 0xff, (word0 >> 16) & 0xff,
                (word0 >>  8) & 0xff,  word0        & 0xff);
    }

    PAL_Runtime_Init();

    /* "Pau de Cego": paint framebuffer solid red to confirm VC4 pipeline is alive.
     * If screen shows red, VC4 is working. If black/nothing, display chain issue. */
    extern uint16_t *framebuffer;
    extern uint32_t  pitch;
    extern uint32_t  fb_width;
    extern uint32_t  fb_height;

    if (framebuffer && pitch && fb_width && fb_height) {
        for (uint32_t y = 0; y < fb_height; y++) {
            uint16_t *row = (uint16_t *)((uintptr_t)framebuffer + y * pitch);
            for (uint32_t x = 0; x < fb_width; x++)
                row[x] = 0x00F8u;  /* red — LE16 RGB565 on big-endian ARM */
        }
        kprintf("[BELA] Pau de Cego: painted %ux%u red (fb=%p pitch=%u)\n",
                (unsigned)fb_width, (unsigned)fb_height,
                (void *)framebuffer, (unsigned)pitch);
    } else {
        kprintf("[BELA] Pau de Cego: framebuffer not ready (fb=%p pitch=%u w=%u h=%u)\n",
                (void *)framebuffer, (unsigned)pitch,
                (unsigned)fb_width, (unsigned)fb_height);
    }

    kprintf("[BELA] Initialized (single-core mode)\n");
}

/* ---------------------------------------------------------------------------
 * Bus dispatch
 * ------------------------------------------------------------------------- */

uint32_t bellatrix_bus_access(uint32_t addr, uint32_t value, int size, int dir)
{
    PAL_Runtime_Poll();

    uint32_t result = 0;
    BellatrixMachine *m = bellatrix_machine_get();

    addr &= 0x00FFFFFFu;

    /* Btrace verbosity control */
    if (addr == BTRACE_CONTROL_ADDR && dir == BUS_WRITE) {
        btrace_set_filter((uint16_t)value);
        return 0;
    }

    /* Writes to chip RAM through overlay (OVL only affects reads) */
    if (dir == BUS_WRITE && addr < CHIP_RAM_SIZE) {
        chip_ram_write(addr, value, size);
        return 0;
    }

    /* Agnus registers (0xDFF000–0xDFF1FF) */
    if (addr >= 0xDFF000u && addr <= 0xDFF1FFu) {
        uint16_t reg = (uint16_t)(addr & 0x1FEu);
        if (dir == BUS_READ) {
            /* Paula-owned read registers */
            if (reg == 0x001Cu || reg == 0x001Eu)
                result = paula_read(&m->paula, addr, (unsigned)size);
            else
                result = agnus_read_reg(&m->agnus, reg);
        } else {
            /* Paula-owned write registers */
            if (reg == 0x009Au || reg == 0x009Cu)
                paula_write(&m->paula, addr, value, (unsigned)size);
            else
                agnus_write_reg(&m->agnus, reg, value, size);
        }
        bellatrix_machine_sync_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    /* CIA-A: 0xBFExxx, A0=1, register in bits [11:8] */
    if (addr >= 0xBFE001u && addr <= 0xBFEF01u && (addr & 0xFFu) == 0x01u) {
        int reg = cia_reg(addr);
        if (dir == BUS_WRITE) {
            cia_write_reg(&m->cia_a, (uint8_t)reg, (uint8_t)value);
            if (reg == 0)
                set_overlay((int)(m->cia_a.pra & 1));
        } else {
            result = cia_read_reg(&m->cia_a, (uint8_t)reg);
        }
        update_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    /* CIA-B: 0xBFDxxx, A0=0, register in bits [11:8] */
    if (addr >= 0xBFD000u && addr <= 0xBFDF00u && (addr & 0xFFu) == 0x00u) {
        int reg = cia_reg(addr);
        if (dir == BUS_WRITE) cia_write_reg(&m->cia_b, (uint8_t)reg, (uint8_t)value);
        else                   result = cia_read_reg(&m->cia_b, (uint8_t)reg);
        update_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    /* Expansion ROM 0xF00000-0xF7FFFF: silent zero, don't flood UART */
    if (addr >= 0xF00000u && addr <= 0xF7FFFFu)
        return 0;

    /* Everything else: unimplemented */
    btrace_log(addr, value, size, dir, 0);
    return 0;
}

/* ---------------------------------------------------------------------------
 * CPU step hook — drives machine timing
 * ------------------------------------------------------------------------- */

void bellatrix_cpu_step(uint32_t cycles)
{
    bellatrix_machine_advance(cycles);
}
