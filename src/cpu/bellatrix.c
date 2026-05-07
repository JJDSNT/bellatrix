// src/cpu/bellatrix.c
//
// Emu68 integration entry point for the Bellatrix chipset emulator.
// Routes every unmapped M68K bus access to the appropriate chipset module.

#include "bellatrix.h"
#include "bridge/bellatrix_bridge.h"
#include "cpu_backend.h"
#include "core/machine.h"
#include "memory/autoconfig.h"
#include "chipset/agnus/agnus.h"
#include "chipset/cia/cia.h"
#include "chipset/denise/denise.h"
#include "chipset/rtc/rtc.h"
#include "debug/cpu_pc.h"
#include "host/pal.h"
#include "io/serial/uart_host.h"
#include "mmu.h"
#include "A64.h"
#include "support.h"
#include "M68k.h"

/* extern'd by start.c ROM loading code */
uint32_t rom_mapped = 0;

/* Reset vectors read from ROM_KVIRT in bellatrix_init(), before any Emu68
 * JIT/cache initialisation.  Used by M68K_StartEmu() BELLATRIX path. */
uint32_t bellatrix_reset_isp = 0;
uint32_t bellatrix_reset_pc = 0;

/* ---------------------------------------------------------------------------
 * Emu68 CpuBackend — wires machine's two CPU callbacks to Emu68 internals
 * ------------------------------------------------------------------------- */

extern struct M68KState *__m68k_state;

static uint32_t emu68_get_pc(void *ctx)
{
    (void)ctx;
    return __m68k_state ? BE32(__m68k_state->PC) : 0u;
}

static void emu68_set_ipl(void *ctx, int level)
{
    (void)ctx;
    PAL_IPL_Set((uint8_t)level);
}

static CpuBackend g_emu68_backend = {
    .ctx = NULL,
    .get_pc = emu68_get_pc,
    .set_ipl = emu68_set_ipl,
};

/* ---------------------------------------------------------------------------
 * Wall-clock driven machine step (strong override of pal_core.c weak stub).
 * Called by PAL_Runtime_Poll() on every bus access.
 * ------------------------------------------------------------------------- */

void bellatrix_runtime_host_step(uint64_t host_now, uint64_t host_freq)
{
    /* The current Emu68 integration is validated with ExecutionLoop-driven
     * machine time. Keep the host-clock runtime path inert until the
     * refactored scheduler is intentionally wired in. */
    (void)host_now;
    (void)host_freq;
}

/* ---------------------------------------------------------------------------
 * Overlay state (CIA-A PRA bit 0 — OVL)
 * ------------------------------------------------------------------------- */

static int s_overlay = 1;

#define BTRACE_CONTROL_ADDR 0xDFFF00u

static inline int cia_reg(uint32_t addr)
{
    return (int)((addr >> 8) & 0xF);
}

/* ---------------------------------------------------------------------------
 * Address normalization / alias collapse
 * ------------------------------------------------------------------------- */

static void __attribute__((unused)) update_ipl(void)
{
    bellatrix_bridge_cpu_sync_ipl();
}

/* ---------------------------------------------------------------------------
 * Overlay switch
 * ------------------------------------------------------------------------- */

static void set_overlay(int new_overlay)
{
    if (new_overlay == s_overlay)
        return;

    {
        extern struct M68KState *__m68k_state;
        uint32_t pc = __m68k_state ? BE32(__m68k_state->PC) : 0u;
        kprintf("[OVL] %d->%d  pc=%08x\n", s_overlay, new_overlay, pc);

        if (!new_overlay)
        {
            /* OVL going low: chip RAM now at 0x000000 — dump key vectors */
            BellatrixMemory *_mem = &bellatrix_machine_get()->memory;
            kprintf("[OVL->RAM] reset isp=%08x pc=%08x\n",
                    (unsigned)bellatrix_chip_read32(_mem, 0x00u),
                    (unsigned)bellatrix_chip_read32(_mem, 0x04u));
            kprintf("[OVL->RAM] vec08=%08x vec0c=%08x vec10=%08x vec14=%08x\n",
                    (unsigned)bellatrix_chip_read32(_mem, 0x08u),
                    (unsigned)bellatrix_chip_read32(_mem, 0x0cu),
                    (unsigned)bellatrix_chip_read32(_mem, 0x10u),
                    (unsigned)bellatrix_chip_read32(_mem, 0x14u));
            kprintf("[OVL->RAM] vec60=%08x vec64=%08x vec68=%08x vec6c=%08x\n",
                    (unsigned)bellatrix_chip_read32(_mem, 0x60u),
                    (unsigned)bellatrix_chip_read32(_mem, 0x64u),
                    (unsigned)bellatrix_chip_read32(_mem, 0x68u),
                    (unsigned)bellatrix_chip_read32(_mem, 0x6cu));
            kprintf("[OVL->RAM] vec70=%08x vec74=%08x vec78=%08x vec7c=%08x\n",
                    (unsigned)bellatrix_chip_read32(_mem, 0x70u),
                    (unsigned)bellatrix_chip_read32(_mem, 0x74u),
                    (unsigned)bellatrix_chip_read32(_mem, 0x78u),
                    (unsigned)bellatrix_chip_read32(_mem, 0x7cu));
        }
    }

    s_overlay = new_overlay;
    bellatrix_memory_set_overlay(bellatrix_machine_memory(), s_overlay);

    if (s_overlay)
    {
        mmu_map(0xf80000, 0x000000, 4096,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 |
                    MMU_READ_ONLY | MMU_ATTR_CACHED,
                0);
    }
    else
    {
        /* Keep page 0 write-protected so KS writes to the vector table
         * (0x00–0xFF) still fault through bellatrix_bus_access and are
         * captured by [VEC-W] logging.  Reads succeed (AF=1 is set).
         * chip_ram_write() uses CHIP_RAM_VIRT and bypasses this mapping. */
        mmu_map(0x000000, 0x000000, 4096,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 |
                    MMU_READ_ONLY | MMU_ATTR_CACHED,
                0);
    }
}

/* ---------------------------------------------------------------------------
 * ROM physical base in Emu68 kernel virtual space
 * ------------------------------------------------------------------------- */

#define CHIP_RAM_KVIRT 0xffffff9000000000ULL
/*
 * For Fast RAM on the real Emu68 target, use the same low identity-mapped
 * alias the CPU/JIT fetch path uses. Using the 0xffffff900... physical alias
 * here can observe stale/divergent data due to aliasing, while ICache fetches
 * are performed from the low 32-bit mapping.
 */
#define FAST_RAM_KVIRT 0x0000000000200000ULL
#define ROM_KVIRT      0xffffff9000f80000ULL

static inline uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* ---------------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------------- */

void bellatrix_init(void)
{
    extern struct M68KState *__m68k_state;

    PAL_Debug_Init(115200);

    autoconfig_enable_z2_ram(BELLATRIX_FAST_RAM_SIZE);
    bellatrix_machine_init(&g_emu68_backend);

    bellatrix_machine_attach_rom((const uint8_t *)ROM_KVIRT, BELLATRIX_ROM_SIZE);
    bellatrix_memory_set_overlay(bellatrix_machine_memory(), 1);

    /* Bellatrix-specific CIA-A defaults: OVL and LED are outputs */
    BellatrixMachine *m = bellatrix_machine_get();
    /*
     * Reuse Emu68's tested RAM backing instead of maintaining a parallel
     * Bellatrix-only Fast RAM buffer. This keeps CPU fetch/store and machine
     * reads/writes coherent on the real target.
     */
    m->memory.chip_ram = (uint8_t *)CHIP_RAM_KVIRT;
    m->memory.chip_ram_size = BELLATRIX_CHIP_RAM_SIZE;
    m->memory.chip_ram_mask = BELLATRIX_CHIP_RAM_MASK;
    m->memory.fast_ram = (uint8_t *)FAST_RAM_KVIRT;
    m->memory.fast_ram_size = BELLATRIX_FAST_RAM_SIZE;
    m->memory.fast_ram_mask = BELLATRIX_FAST_RAM_MASK;
    memset(m->memory.fast_ram, 0, m->memory.fast_ram_size);
    m->cia_a.ddra = 0x03;

    /* ROM diagnostic */
    {
        const uint8_t *rom = (const uint8_t *)ROM_KVIRT;
        kprintf("[BELA] rom_mapped=%d\n", (int)rom_mapped);

        if (rom_mapped)
        {
            uint32_t isp = read_be32(rom);
            uint32_t pc = read_be32(rom + 4);

            /* Capture here — before any Emu68 JIT/cache init runs. */
            bellatrix_reset_isp = isp;
            bellatrix_reset_pc = pc;

            kprintf("[BELA] ROM @ 0xf80000: %02x %02x %02x %02x  %02x %02x %02x %02x\n",
                    rom[0], rom[1], rom[2], rom[3],
                    rom[4], rom[5], rom[6], rom[7]);
            kprintf("[BELA] Reset vectors: ISP=0x%08x  PC=0x%08x\n", isp, pc);

            if (pc < 0x00f80000 || pc > 0x00ffffff)
            {
                kprintf("[BELA] WARNING: PC 0x%08x outside ROM range -- ROM may be corrupt!\n",
                        pc);
            }
        }
        else
        {
            kprintf("[BELA] WARNING: rom_mapped=0 -- M68K will start at PC=0.\n");
        }
    }

    /* Chip RAM: 2 MB R/W */
    mmu_map(0x000000, 0x000000, 0x200000,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    /* Write-trap 0x1000-0x1FFF so MakeFunctions writes to the Exec JMP table
     * are intercepted by bellatrix_bus_access and logged as [JMP-W].
     * chip_ram_write() bypasses this mapping via CHIP_RAM_VIRT. */
    mmu_map(0x1000, 0x1000, 0x1000,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);

    /* ROM overlay at address 0 (overlay=1): reset vectors visible at 0x000000 */
    mmu_map(0xf80000, 0x000000, 4096,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 |
                MMU_READ_ONLY | MMU_ATTR_CACHED,
            0);
    s_overlay = 1;

    /* Fast RAM is plain Emu68-backed RAM on the real target.
     * Keep it directly accessible instead of trapping it through Bellatrix.
     * Bellatrix observes the same backing through FAST_RAM_KVIRT.
     */
    mmu_map(0x200000, 0x200000, 0xA00000,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
    mmu_map(0xC00000, 0xC00000, 0x200000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    mmu_map(0xF00000, 0xF00000, 0x80000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);

    /*
     * Autoconfig window must fault through the Emu68 vectors path. The global
     * 1:1 RAM map established by Emu68 startup would otherwise satisfy
     * 0x00e80000 accesses directly and Bellatrix would never see the config
     * ROM traffic. Re-map the 64 KiB Z2 config page range without AF set so
     * both reads and writes trap cleanly.
     */
    mmu_map(0x00E80000u, 0x00E80000u, 0x00010000u,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    /* Overlay sanity-check */
    if (rom_mapped)
    {
        uint32_t word0;
        asm volatile("mov x9, #0\n\t"
                     "ldr %w0, [x9]\n"
                     : "=r"(word0)
                     :
                     : "x9", "memory");

        kprintf("[BELA] Overlay check virt[0:3]: %02x %02x %02x %02x  "
                "(expect same as ROM bytes above)\n",
                (word0 >> 24) & 0xff, (word0 >> 16) & 0xff,
                (word0 >> 8) & 0xff, word0 & 0xff);
    }

    PAL_Runtime_Init();

#if defined(BELLATRIX_UART_PL011)
#ifndef BELLATRIX_UART_BAUD
#define BELLATRIX_UART_BAUD 115200
#endif
    if (uart_host_open_pl011(&m->uart_host, BELLATRIX_UART_BAUD))
    {
#if defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 1)
        uart_host_set_null_modem_mode(&m->uart_host, NULL_MODEM_LOOPBACK);
#elif defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 2)
        uart_host_set_null_modem_mode(&m->uart_host, NULL_MODEM_LOOPBACK_ONESHOT);
#endif
        kprintf("[SERIAL] PL011 host bridge open at %u baud — GPIO 14/15 (USB-TTL adapter)\n",
                (unsigned)BELLATRIX_UART_BAUD);
#if defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 1)
        kprintf("[SERIAL] internal serial loopback enabled\n");
#elif defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 2)
        kprintf("[SERIAL] internal serial probe loopback enabled\n");
#endif
    }
    else
    {
        kprintf("[SERIAL] PL011 open failed\n");
    }
#else
    if (uart_host_open_pty(&m->uart_host))
    {
        const char *pty_name = uart_host_pty_name(&m->uart_host);
        if (pty_name)
        {
            kprintf("[SERIAL] PTY ready: %s\n", pty_name);
        }
    }
    else if (uart_host_open_miniuart(&m->uart_host, 9600))
    {
#if defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 1)
        uart_host_set_null_modem_mode(&m->uart_host, NULL_MODEM_LOOPBACK);
#elif defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 2)
        uart_host_set_null_modem_mode(&m->uart_host, NULL_MODEM_LOOPBACK_ONESHOT);
#endif
        uint32_t lsr = miniuart_backend_read_lsr();
        kprintf("[SERIAL] mini-UART open at 9600 baud  LSR=0x%08x TX_ready=%s\n",
                lsr, (lsr & 0x20u) ? "yes" : "no (QEMU AUX UART may be unresponsive)");
#if defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 1)
        kprintf("[SERIAL] internal serial loopback enabled\n");
#elif defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 2)
        kprintf("[SERIAL] internal serial probe loopback enabled\n");
#endif
    }
#endif

    /* "Pau de Cego": paint framebuffer solid red to confirm VC4 pipeline is alive.
     * If screen shows red, VC4 is working. If black/nothing, display chain issue. */
    extern uint16_t *framebuffer;
    extern uint32_t pitch;
    extern uint32_t fb_width;
    extern uint32_t fb_height;

    if (framebuffer && pitch && fb_width && fb_height)
    {
        for (uint32_t y = 0; y < fb_height; y++)
        {
            uint16_t *row = (uint16_t *)((uintptr_t)framebuffer + y * pitch);
            for (uint32_t x = 0; x < fb_width; x++)
                row[x] = 0x00F8u; /* red — LE16 RGB565 on big-endian ARM */
        }

        kprintf("[BELA] Pau de Cego: painted %ux%u red (fb=%p pitch=%u)\n",
                (unsigned)fb_width, (unsigned)fb_height,
                (void *)framebuffer, (unsigned)pitch);
    }
    else
    {
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

    addr = bellatrix_bridge_normalize_addr(addr);

    /* First-N trace: log every bus access unconditionally for the first 120
     * calls.  This captures the exact order of accesses at boot and shows
     * where the CPU gets stuck relative to expected CIA/custom writes. */
    {
        static int s_bus_n = 0;
        if (s_bus_n < 120)
        {
            kprintf("[BUS%03d] %s %06x[%d]=%08x\n",
                    s_bus_n,
                    dir == BUS_READ ? "R" : "W",
                    (unsigned)addr, size, (unsigned)value);
            s_bus_n++;
        }
    }

    /* Use the fault-time PC captured by vectors.c (x18 at MMIO fault).
     * Falls back to the stale ctx->PC when called outside a fault context. */
    uint32_t real_pc = g_bellatrix_fault_pc;

    /* Warn if the CPU has strayed into chip RAM — usually means a bad vector. */
    if (real_pc != 0u && real_pc < 0x200000u)
    {
        kprintf("[PC-CHIPMEM] fault_pc=%08x addr=%06x %s size=%d\n",
                (unsigned)real_pc, (unsigned)addr,
                dir == BUS_READ ? "R" : "W", size);
    }

    /* PC trap for a specific ROM range of interest */
    if (real_pc >= 0xfc5e00u && real_pc <= 0xfc5fffu)
    {
        kprintf("[PC-TRAP] pc=%08x addr=%06x %s size=%d val=%08x\n",
                (unsigned)real_pc, (unsigned)addr,
                dir == BUS_READ ? "R" : "W", size, (unsigned)value);
    }

    /* Btrace verbosity control */
    if (addr == BTRACE_CONTROL_ADDR && dir == BUS_WRITE)
    {
        bellatrix_machine_btrace_set_filter((uint16_t)value);
        return 0;
    }

    if (dir == BUS_WRITE)
    {
        if (addr < 0x400u)
        {
            kprintf("[VEC-W] %05x[%d]=%08x\n",
                    (unsigned)addr,
                    size,
                    (unsigned)value);
        }
        else if (addr >= 0x1000u && addr < 0x2000u)
        {
            kprintf("[JMP-W] %05x[%d]=%08x\n",
                    (unsigned)addr,
                    size,
                    (unsigned)value);
        }
        else if (addr >= 0x02368u && addr < 0x02420u)
        {
            kprintf("[CHIPRAM-W] addr=%05x size=%d value=%08x\n",
                    (unsigned)addr,
                    size,
                    (unsigned)value);
        }

        /*
         * The machine owns Amiga behavior:
         * - custom registers
         * - CIA
         * - RTC
         * - Chip RAM
         * - Fast RAM
         * - ROM/overlay reads
         */
        bellatrix_bridge_cpu_write(addr, value, (unsigned)size);

        /*
         * CIA-A PRA bit 0 controls the host MMU overlay mapping.
         * The logical CIA state was already updated by bellatrix_machine_write().
         */
        if (addr >= 0xBFE001u &&
            addr <= 0xBFEF01u &&
            (addr & 0xFFu) == 0x01u &&
            cia_reg(addr) == 0)
        {
            int new_ovl = (int)(m->cia_a.pra & 1u);

            if (new_ovl != s_overlay)
            {
                kprintf("[OVL-TRIG] ciaa_pra_write addr=%08x val=%02x pra=%02x new_ovl=%d\n",
                        (unsigned)addr,
                        (unsigned)(value & 0xFFu),
                        (unsigned)m->cia_a.pra,
                        new_ovl);
            }

            set_overlay(new_ovl);
        }

        return 0;
    }

    result = bellatrix_bridge_cpu_read(addr, (unsigned)size);
    return result;
}

/* ---------------------------------------------------------------------------
 * CPU step hook — drives machine timing
 * ------------------------------------------------------------------------- */

void bellatrix_cpu_step(uint32_t cycles)
{
    bellatrix_bridge_cpu_progress(cycles);
}
