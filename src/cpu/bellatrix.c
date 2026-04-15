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
// ROM physical base in Emu68 kernel virtual space.
// mmu_map(0xf80000, ...) places 512 KB of ROM data here.
// ---------------------------------------------------------------------------
#define ROM_KVIRT  0xffffff9000f80000ULL

// Read a big-endian 32-bit word directly from a kernel-virtual byte pointer.
static inline uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
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

    // -----------------------------------------------------------------------
    // ROM diagnostic — run before the overlay is set up so we read physical
    // memory directly, not through the mapping.
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
            // For OCS/ECS ROMs (KS 1.x–2.x) the "ISP" is 0x1111_4EF9 and
            // for AGA ROMs it is 0x1114_4EF9.  This is the standard Amiga
            // ROM format: bytes 0-1 = ROM type, bytes 2-3 = JMP opcode.
            // The Kickstart overrides SP in its very first instruction.
            // PC is the real entry point: 0x00FC00D2 (KS 1.x) or
            // 0x00F800D2 / 0x00F800FC (KS 2.x+, AGA, AROS).
            if (pc < 0x00f80000 || pc > 0x00ffffff)
                kprintf("[BELA] WARNING: PC 0x%08x is outside ROM range "
                        "(0xf80000-0xffffff) -- ROM may be corrupted!\n", pc);
        } else {
            kprintf("[BELA] WARNING: rom_mapped=0. "
                    "No ROM loaded via initrd -- M68K will start at PC=0.\n");
        }
    }

    // Chip RAM: 2 MB R/W → RPi physical 0x000000
    mmu_map(0x000000, 0x000000, 0x200000,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    // ROM overlay at address 0 (overlay=1): reset vectors visible at 0x000000.
    // M68K_StartEmu reads ISP and PC from virtual address 0; with the overlay
    // active this returns the ROM reset vectors.
    mmu_map(0xf80000, 0x000000, 4096,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    s_overlay = 1;

    // -----------------------------------------------------------------------
    // Chipset trap mappings — map unmapped Amiga address ranges as read-only
    // WITHOUT the Access Flag (no MMU_ACCESS).  In AArch64, a page entry with
    // AF=0 causes an Access Flag Fault on ANY access (read or write), even
    // though the physical backing exists in RAM.  This is required in QEMU
    // (TCG mode) because QEMU does not inject guest Translation Faults for
    // addresses within the guest's physical RAM window; it only injects guest
    // exceptions for Access Flag / Permission faults.
    //
    // With these mappings every CIA, custom-chip and expansion-ROM access
    // reaches bellatrix_bus_access through SYSPageFaultReadHandler /
    // SYSPageFaultWriteHandler, exactly as it does on real hardware.
    //
    // Ranges (physical = virtual — identity map, content irrelevant since the
    // fault handler always returns emulated values):
    //   0x200000-0xBFFFFF  slow RAM, CIA-B ($BFD000), CIA-A ($BFE000)   10 MB
    //   0xC00000-0xDFFFFF  custom chips: Agnus ($DFF000), Paula, etc.    2 MB
    //   0xF00000-0xF7FFFF  expansion ROM check area ($F00000)          512 KB
    // -----------------------------------------------------------------------
    mmu_map(0x200000, 0x200000, 0xA00000,   // 0x200000–0xBFFFFF
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    mmu_map(0xC00000, 0xC00000, 0x200000,   // 0xC00000–0xDFFFFF
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    mmu_map(0xF00000, 0xF00000, 0x80000,    // 0xF00000–0xF7FFFF
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);

    // Overlay sanity-check: virt 0 must now read back the ROM first word.
    // In AArch64 BE mode EL1 and EL0 share TTBR0_EL1, so this read is valid.
    // We use inline asm (ldr from xzr) instead of a C null pointer dereference
    // to avoid GCC's UB trap insertion for known-zero pointer literals.
    if (rom_mapped) {
        uint32_t word0;
        /* xzr cannot be a base register; use a scratch reg forced to 0. */
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

    // ARM generic timer → VBL FIQ at 50 Hz
    PAL_ChipsetTimer_Init(50, NULL);

    PAL_Debug_Print("{\"t\":\"init\",\"msg\":\"Bellatrix Phase 4 ready\"}\n");
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
