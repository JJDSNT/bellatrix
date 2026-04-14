// src/cpu/bellatrix.c
//
// Emu68 integration entry point for the Bellatrix chipset emulator.
// Receives every unmapped M68K bus access from the Emu68 fault handler
// and dispatches it to the appropriate chipset module.

#include "bellatrix.h"
#include "core/btrace.h"
#include "chipset/cia/cia.h"
#include "host/pal.h"
#include "mmu.h"
#include "A64.h"
#include "support.h"

// ---------------------------------------------------------------------------
// rom_mapped — extern'd by start.c ROM loading code.
// PiStorm defines it in vectors.c; Bellatrix defines it here.
// ---------------------------------------------------------------------------
uint32_t rom_mapped = 0;

// ---------------------------------------------------------------------------
// CIA instances
// ---------------------------------------------------------------------------
static CIA_State s_cia_a;
static CIA_State s_cia_b;

// ---------------------------------------------------------------------------
// Overlay state (CIA-A PRA bit 0 — OVL).
// 1 = ROM at 0x000000 (power-on default).
// 0 = chip RAM at 0x000000.
// ---------------------------------------------------------------------------
static int s_overlay = 1;

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
#define BTRACE_CONTROL_ADDR 0xDFFF00

// Extract CIA register index 0-15 from address bits [11:8].
static inline int cia_reg(uint32_t addr) {
    return (int)((addr >> 8) & 0xF);
}

// ---------------------------------------------------------------------------
// IPL calculation — Phase 2 stub.
// Phase 3 replaces this with INTREQ/INTENA routing via Paula.
// ---------------------------------------------------------------------------
static void update_ipl(void)
{
    int ipl = 0;
    if (cia_irq_pending(&s_cia_b)) ipl = 6;          // CIA-B → EXTER → IPL 6
    if (cia_irq_pending(&s_cia_a) && ipl < 2) ipl = 2; // CIA-A → PORTS → IPL 2

    if (ipl > 0) PAL_IPL_Set((uint8_t)ipl);
    else         PAL_IPL_Clear();
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
// Initialisation
// ---------------------------------------------------------------------------
void bellatrix_init(void)
{
    PAL_Debug_Init(115200);
    btrace_init();
    cia_init(&s_cia_a);
    cia_init(&s_cia_b);

    s_cia_a.pra  = 0xFF;
    s_cia_a.ddra = 0x03;  // OVL (bit 0) and LED (bit 1) are outputs
    s_cia_b.pra  = 0xFF;

    // Chip RAM: 2 MB R/W → RPi physical 0x000000
    mmu_map(0x000000, 0x000000, 0x200000,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    // ROM overlay at address 0 (overlay = 1): reset vectors visible at 0x000000
    mmu_map(0xf80000, 0x000000, 4096,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);

    s_overlay = 1;
    PAL_Debug_Print("{\"t\":\"init\",\"msg\":\"Bellatrix Phase 2 ready\"}\n");
}

// ---------------------------------------------------------------------------
// Bus dispatch
// ---------------------------------------------------------------------------
uint32_t bellatrix_bus_access(uint32_t addr, uint32_t value, int size, int dir)
{
    uint32_t result = 0;

    if (addr == BTRACE_CONTROL_ADDR && dir == BUS_WRITE) {
        btrace_set_filter((uint16_t)value);
        return 0;
    }

    // CIA-A: 0xBFExxx where x byte = 0x01 (A0=1), register in [11:8]
    if (addr >= 0xBFE001 && addr <= 0xBFEF01 && (addr & 0xFF) == 0x01) {
        int reg = cia_reg(addr);
        if (dir == BUS_WRITE) {
            cia_write(&s_cia_a, reg, (uint8_t)value);
            if (reg == CIA_REG_PRA)
                set_overlay((int)(s_cia_a.pra & 1));
        } else {
            result = cia_read(&s_cia_a, reg);
        }
        update_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    // CIA-B: 0xBFDxxx where x byte = 0x00 (A0=0), register in [11:8]
    if (addr >= 0xBFD000 && addr <= 0xBFDF00 && (addr & 0xFF) == 0x00) {
        int reg = cia_reg(addr);
        if (dir == BUS_WRITE) cia_write(&s_cia_b, reg, (uint8_t)value);
        else                   result = cia_read(&s_cia_b, reg);
        update_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    // Everything else: unimplemented
    btrace_log(addr, value, size, dir, 0);
    return 0;
}
