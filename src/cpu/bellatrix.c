// src/cpu/bellatrix.c
//
// Emu68 integration entry point for the Bellatrix chipset emulator.
// Routes every unmapped M68K bus access to the appropriate chipset module.

#include "bellatrix.h"
#include "core/btrace.h"
#include "chipset/cia/cia.h"
#include "chipset/agnus/agnus.h"
#include "host/pal.h"
#include "mmu.h"
#include "A64.h"
#include "support.h"

// ---------------------------------------------------------------------------
// rom_mapped — extern'd by start.c ROM loading code.
// ---------------------------------------------------------------------------
uint32_t rom_mapped = 0;

// ---------------------------------------------------------------------------
// CIA instances
// ---------------------------------------------------------------------------
static CIA_State s_cia_a;
static CIA_State s_cia_b;

// ---------------------------------------------------------------------------
// Overlay state (CIA-A PRA bit 0 — OVL).
// 1 = ROM at 0x000000  (power-on default)
// 0 = chip RAM at 0x000000
// ---------------------------------------------------------------------------
static int s_overlay = 1;

// ---------------------------------------------------------------------------
// Addresses
// ---------------------------------------------------------------------------
#define BTRACE_CONTROL_ADDR  0xDFFF00   // Write to set btrace filter at runtime

// Extract CIA register index 0-15 from bits [11:8].
static inline int cia_reg(uint32_t addr) {
    return (int)((addr >> 8) & 0xF);
}

// ---------------------------------------------------------------------------
// VBL tick — called by agnus_vbl_fire() (which is called from FIQ handler)
// ---------------------------------------------------------------------------
void bellatrix_cia_vbl_tick(void)
{
    cia_vbl_tick(&s_cia_a);
    cia_vbl_tick(&s_cia_b);
}

// ---------------------------------------------------------------------------
// IPL recalculation — routes CIA interrupts through INTREQ then to the JIT.
// ---------------------------------------------------------------------------
static void update_ipl(void)
{
    // CIA-A → PORTS (INT2), CIA-B → EXTER (INT6)
    if (cia_irq_pending(&s_cia_a)) agnus_intreq_set(INT_PORTS);
    if (cia_irq_pending(&s_cia_b)) agnus_intreq_set(INT_EXTER);
    // agnus_intreq_set already calls notify_ipl → PAL_IPL_Set / PAL_IPL_Clear
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
    agnus_init();
    cia_init(&s_cia_a);
    cia_init(&s_cia_b);

    s_cia_a.pra  = 0xFF;
    s_cia_a.ddra = 0x03;  // OVL (bit 0) and LED (bit 1) are outputs
    s_cia_b.pra  = 0xFF;

    // Chip RAM: 2 MB R/W → RPi physical 0x000000
    mmu_map(0x000000, 0x000000, 0x200000,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    // ROM overlay at address 0 (overlay=1): reset vectors visible at 0x000000
    mmu_map(0xf80000, 0x000000, 4096,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    s_overlay = 1;

    // ARM generic timer → VBL FIQ at 50 Hz
    PAL_ChipsetTimer_Init(50, NULL);

    PAL_Debug_Print("{\"t\":\"init\",\"msg\":\"Bellatrix Phase 3 ready\"}\n");
}

// ---------------------------------------------------------------------------
// Chip RAM direct write (bypasses MMU read-only overlay mapping).
// On the real Amiga, OVL only affects reads — writes to 0x000000-0x1FFFFF
// always go to chip RAM regardless of the overlay state.
// Physical chip RAM base in Emu68's kernel virtual space:
#define CHIP_RAM_VIRT  0xffffff9000000000ULL
#define CHIP_RAM_SIZE  0x200000UL

static void chip_ram_write(uint32_t addr, uint32_t value, int size)
{
    uintptr_t virt = CHIP_RAM_VIRT + addr;
    switch (size) {
    case 1: *(volatile uint8_t  *)virt = (uint8_t) value; break;
    case 2: *(volatile uint16_t *)virt = (uint16_t)value; break;
    case 4: *(volatile uint32_t *)virt = value;            break;
    }
}

// ---------------------------------------------------------------------------
// Bus dispatch
// ---------------------------------------------------------------------------
uint32_t bellatrix_bus_access(uint32_t addr, uint32_t value, int size, int dir)
{
    uint32_t result = 0;

    // ---- Btrace verbosity control ----
    if (addr == BTRACE_CONTROL_ADDR && dir == BUS_WRITE) {
        btrace_set_filter((uint16_t)value);
        return 0;
    }

    // ---- Writes to chip RAM that faulted due to ROM overlay (read-only page) ----
    // OVL only affects reads. Writes always target chip RAM.
    if (dir == BUS_WRITE && addr < CHIP_RAM_SIZE) {
        chip_ram_write(addr, value, size);
        return 0;  // don't log — this is normal chip RAM write via overlay path
    }

    // ---- Agnus registers (0xDFF000–0xDFF1FF) ----
    if (addr >= 0xDFF000 && addr <= 0xDFF1FF) {
        if (dir == BUS_READ) {
            result = agnus_read(addr);
        } else {
            agnus_write(addr, value, size);
        }
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    // ---- CIA-A: 0xBFExxx, A0=1, reg in [11:8] ----
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

    // ---- CIA-B: 0xBFDxxx, A0=0, reg in [11:8] ----
    if (addr >= 0xBFD000 && addr <= 0xBFDF00 && (addr & 0xFF) == 0x00) {
        int reg = cia_reg(addr);
        if (dir == BUS_WRITE) cia_write(&s_cia_b, reg, (uint8_t)value);
        else                   result = cia_read(&s_cia_b, reg);
        update_ipl();
        btrace_log(addr, dir == BUS_WRITE ? value : result, size, dir, 1);
        return result;
    }

    // ---- Everything else: unimplemented ----
    btrace_log(addr, value, size, dir, 0);
    return 0;
}
