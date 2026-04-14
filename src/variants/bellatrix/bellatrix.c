// src/variants/bellatrix/bellatrix.c
//
// Entry point for the Bellatrix chipset emulator.
//
// Phase 0: bus trace stubs.
// Phase 1: chip RAM + ROM overlay.
// Phase 2: CIA-A and CIA-B register dispatch.

#include "bellatrix.h"
#include "chipset/btrace.h"
#include "chipset/cia.h"
#include "platform/pal.h"
#include "mmu.h"
#include "A64.h"
#include "support.h"

// ---------------------------------------------------------------------------
// rom_mapped — referenced as 'extern' by start.c's ROM loading code.
// In the PiStorm variant this lives in vectors.c; for Bellatrix we define it.
// ---------------------------------------------------------------------------
uint32_t rom_mapped = 0;

// ---------------------------------------------------------------------------
// CIA instances
// ---------------------------------------------------------------------------
static CIA_State s_cia_a;
static CIA_State s_cia_b;

// ---------------------------------------------------------------------------
// Overlay state (CIA-A PRA bit 0 — OVL).
// 1 = ROM visible at 0x000000 (power-on default).
// 0 = chip RAM visible at 0x000000.
// ---------------------------------------------------------------------------
static int s_overlay = 1;

// ---------------------------------------------------------------------------
// Special addresses
// ---------------------------------------------------------------------------
#define BTRACE_CONTROL_ADDR 0xDFFF00  // Write to set btrace filter

// Amiga CIA-A base: 0xBFE001 (A0=1, register in A8-A11).
// Amiga CIA-B base: 0xBFD000 (A0=0, register in A8-A11).
#define CIA_A_BASE  0xBFE001
#define CIA_B_BASE  0xBFD000

static inline int is_cia_a(uint32_t addr) {
    // CIA-A: A8-A12 select chip, A0 must be 1.
    // Mask: check upper 12 bits of relevant page and odd byte.
    return ((addr & 0xFF000F) == 0xBFE001) ||
           ((addr & 0xFFFF00) == 0xBFE000 && (addr & 1) == 1 && addr >= 0xBFE001 && addr <= 0xBFEF01);
}

static inline int is_cia_b(uint32_t addr) {
    return ((addr & 0xFFFF00) == 0xBFD000 && (addr & 1) == 0 && addr >= 0xBFD000 && addr <= 0xBFDF00);
}

// Extract register index 0-15 from A8-A11.
static inline int cia_reg(uint32_t addr) {
    return (int)((addr >> 8) & 0xF);
}

// ---------------------------------------------------------------------------
// IPL: update the M68K interrupt level based on pending CIA interrupts.
// Phase 2 stub: directly sets IPL from CIA IRQ flags.
// Phase 3 replaces this with proper INTREQ/INTENA routing via Paula.
// ---------------------------------------------------------------------------
static void update_ipl(void)
{
    // CIA-A → PORTS (INT2) → IPL 2
    // CIA-B → EXTER (INT6) → IPL 6
    int ipl = 0;
    if (cia_irq_pending(&s_cia_b)) ipl = 6;
    if (cia_irq_pending(&s_cia_a)) ipl = (ipl < 2) ? 2 : ipl;

    if (ipl > 0)
        PAL_IPL_Set((uint8_t)ipl);
    else
        PAL_IPL_Clear();
}

// ---------------------------------------------------------------------------
// Initialisation
// ---------------------------------------------------------------------------

void bellatrix_init(void)
{
    PAL_Debug_Init(115200);
    btrace_init();
    cia_init(&s_cia_a);
    cia_init(&s_cia_b);

    // CIA-A PRA bit 0 (OVL) defaults to 1 (ROM at 0x000000).
    // CIA-A DDRA: bit 0 is output (driven by hardware), bits 2-6 are inputs.
    s_cia_a.pra  = 0xFF;
    s_cia_a.ddra = 0x03; // bits 0 (OVL) and 1 (LED) are outputs

    // CIA-B PRA default: all drives deselected, motors off.
    s_cia_b.pra  = 0xFF;

    // Map chip RAM: 2 MB R/W at Amiga address 0x000000 → RPi physical 0x000000.
    mmu_map(0x000000, 0x000000, 0x200000,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    // Map ROM at address 0 (overlay = 1): first 4 KB of Kickstart in user space.
    // M68K_StartEmu reads the reset vectors (ISP at [0], PC at [4]) from here.
    mmu_map(0xf80000, 0x000000, 4096,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);

    s_overlay = 1;

    PAL_Debug_Print("{\"t\":\"init\",\"msg\":\"Bellatrix Phase 2 ready\"}\n");
}

// ---------------------------------------------------------------------------
// Overlay switch (called from CIA-A PRA write, bit 0)
// ---------------------------------------------------------------------------

static void set_overlay(int new_overlay)
{
    if (new_overlay == s_overlay) return;
    s_overlay = new_overlay;

    if (s_overlay) {
        // Restore ROM shadow at 0x000000 (read-only, first 4 KB).
        mmu_map(0xf80000, 0x000000, 4096,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    } else {
        // Expose chip RAM at 0x000000 (read/write).
        mmu_map(0x000000, 0x000000, 4096,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
    }
}

// ---------------------------------------------------------------------------
// Bus dispatch
// ---------------------------------------------------------------------------

uint32_t bellatrix_bus_access(uint32_t addr, uint32_t value, int size, int dir)
{
    uint32_t result = 0;

    // ---- Btrace verbosity control register ----
    if (addr == BTRACE_CONTROL_ADDR && dir == BUS_WRITE) {
        btrace_set_filter((uint16_t)value);
        return 0;
    }

    // ---- CIA-A ----
    if (addr >= 0xBFE001 && addr <= 0xBFEF01 && (addr & 0xFF) == 0x01) {
        int reg = cia_reg(addr);
        if (dir == BUS_WRITE) {
            uint8_t prev_pra = s_cia_a.pra;
            cia_write(&s_cia_a, reg, (uint8_t)value);
            // Handle overlay switch on PRA write (bit 0 = OVL).
            if (reg == CIA_REG_PRA) {
                set_overlay((int)(s_cia_a.pra & 1));
                (void)prev_pra;
            }
        } else {
            result = cia_read(&s_cia_a, reg);
        }
        update_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1 /* implemented */);
        return result;
    }

    // ---- CIA-B ----
    if (addr >= 0xBFD000 && addr <= 0xBFDF00 && (addr & 0xFF) == 0x00) {
        int reg = cia_reg(addr);
        if (dir == BUS_WRITE) {
            cia_write(&s_cia_b, reg, (uint8_t)value);
        } else {
            result = cia_read(&s_cia_b, reg);
        }
        update_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1 /* implemented */);
        return result;
    }

    // ---- Everything else: unimplemented ----
    btrace_log(addr, value, size, dir, 0 /* unimplemented */);
    return 0;
}
