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

// ---------------------------------------------------------------------------
// rom_mapped — extern'd by start.c ROM loading code.
// ---------------------------------------------------------------------------
uint32_t rom_mapped = 0;

// ---------------------------------------------------------------------------
// Wall-clock driven machine step (strong override of pal_core.c weak stub).
//
// Called by PAL_Runtime_Poll() on every bus access.  Converts host counter
// delta to M68K cycles (PAL: 7.09 MHz) and advances machine_step.
// ---------------------------------------------------------------------------
#define BELLATRIX_M68K_CLOCK_HZ 7093790ULL

void bellatrix_runtime_host_step(uint64_t host_now, uint64_t host_freq)
{
    static uint64_t s_last = 0;

    if (!s_last) { s_last = host_now; return; }

    uint64_t delta = host_now - s_last;
    s_last = host_now;

    // Cap at 1 host-counter second to absorb startup / debugger pauses.
    if (host_freq && delta > host_freq) delta = host_freq;

    if (!host_freq) return;

    uint64_t m68k_cycles = (delta * BELLATRIX_M68K_CLOCK_HZ) / host_freq;
    if (m68k_cycles) {
        BellatrixMachine *m = bellatrix_machine_get();
        machine_step(m, m68k_cycles);
    }
}

// ---------------------------------------------------------------------------
// Overlay state (CIA-A PRA bit 0 — OVL).
// 1 = ROM at 0x000000  (power-on default)
// 0 = chip RAM at 0x000000
// ---------------------------------------------------------------------------
static int s_overlay = 1;

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
#define BTRACE_CONTROL_ADDR  0xDFFF00u

// Derive CIA register index 0-15 from bits [11:8].
static inline int cia_reg(uint32_t addr) {
    return (int)((addr >> 8) & 0xF);
}

// ---------------------------------------------------------------------------
// IPL recalculation — routes CIA interrupts through INTREQ then to the JIT.
// ---------------------------------------------------------------------------
static void update_ipl(void)
{
    BellatrixMachine *m = bellatrix_machine_get();
    if (cia_irq_pending(&m->cia_a)) agnus_intreq_set(&m->agnus, INT_PORTS);
    if (cia_irq_pending(&m->cia_b)) agnus_intreq_set(&m->agnus, INT_EXTER);
}

// ---------------------------------------------------------------------------
// Overlay switch
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// ROM physical base in Emu68 kernel virtual space.
// ---------------------------------------------------------------------------
#define ROM_KVIRT  0xffffff9000f80000ULL

static inline uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

// ---------------------------------------------------------------------------
// Chip RAM direct write (bypasses MMU read-only overlay mapping).
// On the real Amiga, OVL only affects reads — writes to 0x000000-0x1FFFFF
// always go to chip RAM regardless of overlay state.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------
void bellatrix_init(void)
{
    extern struct M68KState *__m68k_state;

    PAL_Debug_Init(115200);
    btrace_init();
    denise_init();

    bellatrix_machine_init(__m68k_state);

    BellatrixMachine *m = bellatrix_machine_get();

    agnus_init(&m->agnus);
    cia_init(&m->cia_a);
    cia_init(&m->cia_b);

    m->cia_a.pra  = 0xFF;
    m->cia_a.ddra = 0x03;  // OVL (bit 0) and LED (bit 1) are outputs
    m->cia_b.pra  = 0xFF;

    // -----------------------------------------------------------------------
    // ROM diagnostic
    // -----------------------------------------------------------------------
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

    // Chip RAM: 2 MB R/W
    mmu_map(0x000000, 0x000000, 0x200000,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    // ROM overlay at address 0 (overlay=1): reset vectors visible at 0x000000.
    mmu_map(0xf80000, 0x000000, 4096,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    s_overlay = 1;

    // Chipset trap mappings — AF=0 pages so every CIA/custom access faults.
    // Ranges (identity map, content irrelevant — fault handler returns emulated values):
    //   0x200000-0xBFFFFF  slow RAM, CIA-B ($BFD000), CIA-A ($BFE000)   10 MB
    //   0xC00000-0xDFFFFF  custom chips: Agnus, Denise, Paula, etc.      2 MB
    //   0xF00000-0xF7FFFF  expansion ROM check area                    512 KB
    mmu_map(0x200000, 0x200000, 0xA00000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    mmu_map(0xC00000, 0xC00000, 0x200000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    mmu_map(0xF00000, 0xF00000, 0x80000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);

    // Overlay sanity-check: virt 0 must read back the ROM first word.
    if (rom_mapped) {
        uint32_t word0;
        asm volatile("mov x9, #0\n\t"
                     "ldr %w0, [x9]\n"
                     : "=r"(word0) : : "x9", "memory");
        kprintf("[BELA] Overlay check virt[0:3]: %02x %02x %02x %02x  "
                "(expect same as ROM bytes above)\n",
                (word0 >> 24) & 0xff,
                (word0 >> 16) & 0xff,
                (word0 >>  8) & 0xff,
                 word0        & 0xff);
    }

    PAL_Runtime_Init();

    kprintf("[BELA] Initialized (single-core mode)\n");
}

// ---------------------------------------------------------------------------
// Bus dispatch
// ---------------------------------------------------------------------------

uint32_t bellatrix_bus_access(uint32_t addr, uint32_t value, int size, int dir)
{
    // Drive machine time from wall clock on every bus access (single-core mode).
    PAL_Runtime_Poll();

    uint32_t result = 0;
    BellatrixMachine *m = bellatrix_machine_get();

    // Mask to 24-bit Amiga address bus
    addr &= 0x00FFFFFFu;

    // ---- Btrace verbosity control ----
    if (addr == BTRACE_CONTROL_ADDR && dir == BUS_WRITE) {
        btrace_set_filter((uint16_t)value);
        return 0;
    }

    // ---- Writes to chip RAM through overlay (OVL makes 0x0-0xFFF read-only) ----
    // OVL only affects reads. Writes always target chip RAM.
    if (dir == BUS_WRITE && addr < CHIP_RAM_SIZE) {
        chip_ram_write(addr, value, size);
        return 0;
    }

    // ---- Agnus registers (0xDFF000–0xDFF1FF) ----
    if (addr >= 0xDFF000u && addr <= 0xDFF1FFu) {
        uint16_t reg = (uint16_t)(addr & 0x1FEu);
        if (dir == BUS_READ) {
            result = agnus_read_reg(&m->agnus, reg);
        } else {
            agnus_write_reg(&m->agnus, reg, value, size);
        }
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    // ---- CIA-A: 0xBFExxx, A0=1, register in bits [11:8] ----
    if (addr >= 0xBFE001u && addr <= 0xBFEF01u && (addr & 0xFFu) == 0x01u) {
        int reg = cia_reg(addr);
        if (dir == BUS_WRITE) {
            cia_write_reg(&m->cia_a, (uint8_t)reg, (uint8_t)value);
            if (reg == 0)  // CIA_REG_PRA — OVL bit
                set_overlay((int)(m->cia_a.pra & 1));
        } else {
            result = cia_read_reg(&m->cia_a, (uint8_t)reg);
        }
        update_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    // ---- CIA-B: 0xBFDxxx, A0=0, register in bits [11:8] ----
    if (addr >= 0xBFD000u && addr <= 0xBFDF00u && (addr & 0xFFu) == 0x00u) {
        int reg = cia_reg(addr);
        if (dir == BUS_WRITE) cia_write_reg(&m->cia_b, (uint8_t)reg, (uint8_t)value);
        else                   result = cia_read_reg(&m->cia_b, (uint8_t)reg);
        update_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    // ---- Everything else: unimplemented ----
    btrace_log(addr, value, size, dir, 0);
    return 0;
}

// ---------------------------------------------------------------------------
// CPU step hook — drives machine timing
// ---------------------------------------------------------------------------
void bellatrix_cpu_step(uint32_t cycles)
{
    BellatrixMachine *m = bellatrix_machine_get();
    machine_step(m, (uint64_t)cycles);
}
