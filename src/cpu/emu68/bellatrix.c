// src/cpu/emu68/bellatrix.c
//
// Emu68 integration entry point for the Bellatrix chipset emulator.
// Routes every unmapped M68K bus access to the appropriate chipset module.

#include "cpu/emu68/bellatrix.h"
#include "cpu/emu68/bellatrix_profile.h"
#include "cpu/cpu_bridge.h"
#include "runtime/runtime.h"
#ifdef BELLATRIX_LAUNCHER
#include "launcher/launcher.h"
#endif
#include <stdatomic.h>
#include "cpu/cpu_backend.h"
#include "cpu/musashi/musashi_backend.h"
#include "machine/machine.h"
#include "rigel/rigel_custom.h"
#include "rigel/rigel_cia.h"
#include "rigel/rigel_mmio.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "debug/cpu_pc.h"
#include "host/pal.h"
#include "host/raspi3/hdmi_audio.h"
#include "host/raspi3/console_log.h"
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

extern struct M68KState *__m68k_state;

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

static CpuBackend g_emu68_backend __attribute__((unused)) = {
    .ctx = NULL,
    .get_pc = emu68_get_pc,
    .set_ipl = emu68_set_ipl,
    .reset = NULL,
    .run = NULL,
};

#if defined(BELLATRIX_USE_MUSASHI_CPU) && BELLATRIX_USE_MUSASHI_CPU
static CpuBackend *bellatrix_selected_cpu_backend(void)
{
    return bellatrix_musashi_backend_get();
}
#else
static CpuBackend *bellatrix_selected_cpu_backend(void)
{
    return &g_emu68_backend;
}
#endif

static void bellatrix_singlecore_advance_cpu_cycles(uint32_t cycles);

static void bellatrix_runtime_poll_from_emu68(void)
{
    static uint32_t poll_countdown;

    if (PAL_Core_IsMulticoreEnabled())
        return;

    if (poll_countdown++ & 0x3ffu)
        return;

    PAL_Runtime_Poll();
}

int bellatrix_cpu_backend_owns_execution_loop(void)
{
    CpuBackend *backend = bellatrix_selected_cpu_backend();
    return backend && backend->run && backend->reset;
}

void bellatrix_run_selected_cpu_backend(void)
{
    CpuBackend *backend = bellatrix_selected_cpu_backend();

    if (!backend || !backend->run || !backend->reset) {
        return;
    }

    cpu_backend_reset(backend);

    for (;;) {
        uint32_t ran = cpu_backend_run(backend, 454u);
        uint32_t cycles = ran > 0u ? ran : 454u;

        if (PAL_Core_IsMulticoreEnabled()) {
            /* Multicore: Core 2 (chipset) and Core 3 (IO) run independently;
             * publish cross-core like the Emu68 JIT path does instead of
             * stepping Rigel synchronously on this (CPU) core. */
            bellatrix_bridge_publish_cpu_cycles(cycles);
        } else {
            bellatrix_singlecore_advance_cpu_cycles(cycles);
            /* The Musashi loop never goes through bellatrix_bus_access (that is
             * the Emu68 JIT fault hook), so the single-core IO service point
             * (USB/BT via PAL_Runtime_Poll's ~1ms throttle) must live here. */
            PAL_Runtime_Poll();
        }
    }
}

/* ---------------------------------------------------------------------------
 * Multicore runtime state.
 *
 * Cycle/MMIO synchronisation between the CPU core (Core 1) and the chipset
 * core (Core 2) is handled by core_chipset.c (cycle target + access lock) —
 * see core_chipset_lock_acquire/release(), called from cpu_bridge.c.
 * ------------------------------------------------------------------------- */

/* Per-core runtime objects — advanced by Core 2 via bellatrix_runtime_host_step(). */
BellatrixRuntime g_runtime;

/* ---------------------------------------------------------------------------
 * Core 0 (boot/Machine) → Core 1 (CPU) handoff.
 *
 * Single-core: entry() runs forever inline on the boot core, unchanged from
 * the pre-multicore boot path.
 * Multicore: Core 1 is launched to run entry(); Core 0 parks as the light
 * Machine/scheduler arbiter described in multicore.md — it has no recurring
 * work of its own because the CPU<->chipset protocol already lives inside
 * the CPU's MMIO fault path and the chipset's step function.
 * ------------------------------------------------------------------------- */
void bellatrix_launch_cpu_and_park(void (*entry)(void))
{
    if (!entry)
        return;

    if (!PAL_Core_IsMulticoreEnabled()) {
        entry();
        return;
    }

    PAL_Core_LaunchCpu(entry);

    for (;;)
        asm volatile("wfe");
}

/* ---------------------------------------------------------------------------
 * Emu68/Musashi single-core CPU progress.
 *
 * This path advances the machine synchronously on the caller's core.  Do not
 * route it through cpu_bridge/runtime publish symbols; those are intentionally
 * owned by the generic runtime path and may resolve to the multicore publisher.
 * ------------------------------------------------------------------------- */
static void bellatrix_singlecore_advance_cpu_cycles(uint32_t cycles)
{
#if BELLATRIX_PROFILE_ENABLED
    uint64_t t0 = bprof_now();
    bellatrix_machine_advance(cycles);
    bprof_record(&g_bprof.advance_time, bprof_now() - t0);
    g_bprof.advance_stats.calls++;
    g_bprof.advance_stats.cpu_cycles_total += cycles;
    if (cycles < g_bprof.advance_stats.cpu_cycles_min)
        g_bprof.advance_stats.cpu_cycles_min = cycles;
    if (cycles > g_bprof.advance_stats.cpu_cycles_max)
        g_bprof.advance_stats.cpu_cycles_max = cycles;
#else
    bellatrix_machine_advance(cycles);
#endif
}

void bellatrix_emu68_report_jit_progress(uint64_t insn_count, uint32_t pc)
{
    static uint64_t s_prev_insn_count;
#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
    static uint32_t s_last_pc;
    static uint32_t s_same_pc_reports;
#endif
    uint32_t cycles = 0u;

    g_bellatrix_exec_pc = pc;

    if (!s_prev_insn_count || insn_count <= s_prev_insn_count) {
        s_prev_insn_count = insn_count;
        cycles = 8u;
    } else {
        uint64_t delta = insn_count - s_prev_insn_count;
        s_prev_insn_count = insn_count;
        cycles = (uint32_t)delta * 8u;
    }

    if (cycles > 0u) {
        if (PAL_Core_IsMulticoreEnabled())
            bellatrix_bridge_publish_cpu_cycles(cycles);
        else
            bellatrix_singlecore_advance_cpu_cycles(cycles);
    }

#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
    if (pc == s_last_pc) {
        if (++s_same_pc_reports == 512u || s_same_pc_reports == 4096u ||
            s_same_pc_reports == 32768u || s_same_pc_reports == 262144u) {
            RigelContext *ctx = bellatrix_machine_rigel_ctx();
            uint16_t vposr = ctx ? rigel_custom_read16(ctx, RIGEL_REG_VPOSR) : 0u;
            uint16_t vhposr = ctx ? rigel_custom_read16(ctx, RIGEL_REG_VHPOSR) : 0u;
            uint16_t intena = ctx ? rigel_custom_read16(ctx, RIGEL_REG_INTENAR) : 0u;
            uint16_t intreq = ctx ? rigel_custom_read16(ctx, RIGEL_REG_INTREQR) : 0u;
            BellatrixMachine *machine = bellatrix_machine_get();
            uint64_t tick_cck = machine ? machine->tick_count : 0u;
            uint32_t int32 = __m68k_state ? __m68k_state->INT32 : 0u;
            uint32_t ipl = __m68k_state ? __m68k_state->INT.IPL : 0u;

            kprintf("[EMU68-STUCK] pc=%08x repeats=%u insn=%llu cycles=%u "
                    "INT32=%08x IPL=%u INTENA=%04x INTREQ=%04x beam=%04x/%04x cck=%llu\n",
                    (unsigned)pc,
                    (unsigned)s_same_pc_reports,
                    (unsigned long long)insn_count,
                    (unsigned)cycles,
                    (unsigned)int32,
                    (unsigned)ipl,
                    (unsigned)intena,
                    (unsigned)intreq,
                    (unsigned)vposr,
                    (unsigned)vhposr,
                    (unsigned long long)tick_cck);
        }
    } else {
        s_last_pc = pc;
        s_same_pc_reports = 0u;
    }
#endif

    bellatrix_runtime_poll_from_emu68();
}

/* ---------------------------------------------------------------------------
 * Strong overrides: per-core chipset advance steps.
 *
 * bellatrix_runtime_host_step  — Core 1, full chipset.
 * bellatrix_runtime_io_step    — Core 3, physical IO.
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * Strong override: MMIO barrier — called from PAL_Runtime_MmioBarrier().
 * Not needed here since bus_access acquires the chipset lock directly.
 * ------------------------------------------------------------------------- */
void bellatrix_runtime_mmio_barrier(void)
{
    asm volatile("dmb ish" ::: "memory");
}

/* ---------------------------------------------------------------------------
 * Overlay state (CIA-A PRA bit 0 — OVL)
 * ------------------------------------------------------------------------- */

static int s_overlay = 1;

#define BTRACE_CONTROL_ADDR 0xDFFF00u
#define ROM_OVERLAY_BASE    0x00E00000u

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

static void apply_overlay_map(int overlay_enabled)
{
    if (overlay_enabled)
    {
        mmu_map(ROM_OVERLAY_BASE, 0x000000, BELLATRIX_ROM_SIZE,
                MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 |
                    MMU_READ_ONLY | MMU_ATTR_CACHED,
                0);
        return;
    }

    /* Chip RAM is fully R/W: all pages 0x000000-0x07FFFF map directly to
     * physical chip RAM.  No write-trap for pages 0-1 — the alias between
     * CHIP_RAM_KVIRT (EL1 write) and the low virtual address (EL0 read)
     * caused store-buffer coherency failures for programs testing $000400. */
    mmu_map(0x000000, 0x000000, BELLATRIX_ROM_SIZE,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
}

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
    apply_overlay_map(s_overlay);
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
#define FAST_RAM_KVIRT  0x0000000000200000ULL
#define ROM_KVIRT       0xffffff9000f80000ULL
/* Extended ROM: first 512 KB of 1 MB ROMs (AROS modules) at physical 0xe00000 */
#define ROM_EXT_KVIRT   0xffffff9000e00000ULL

static inline uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

#if BELLATRIX_ENABLE_BTSTACK
#include "io/bluetooth/bt_host.h"
#include "hal_time_ms.h"
static void bellatrix_init_bluetooth(BellatrixRuntime *rt, BellatrixMachine *m)
{
    if (!rt || !m) {
        return;
    }

    /* Bring BT up only after PAL runtime/timer state exists. PL011 belongs
     * to Bluetooth unconditionally — Paula's serial backends (PTY/mini-UART/
     * log) never touch it, so there's no ownership conflict to check here. */
    if (!bt_host_init(&rt->bluetooth)) {
        kprintf("[BT] init failed\n");
        return;
    }
}
#endif

/* ---------------------------------------------------------------------------
 * Initialisation
 * ------------------------------------------------------------------------- */

void bellatrix_init(void)
{
    extern struct M68KState *__m68k_state;
    CpuBackend *cpu_backend;

    PAL_Debug_Init(115200);
    bellatrix_emu68_boards_reset();
    cpu_backend = bellatrix_selected_cpu_backend();

#if defined(BELLATRIX_USE_MUSASHI_CPU) && BELLATRIX_USE_MUSASHI_CPU
    bellatrix_musashi_backend_init();
#endif

#if !BELLATRIX_ENABLE_EMU68_BOARDS
    /* Legacy non-boards path: Bellatrix owns the Zorro II protocol when
     * Emu68 board support is not compiled in. Configure Z2 RAM before
     * machine_init() so the board is present from the first bus reset. */
#ifndef BELLATRIX_LEGACY_Z2_RAM_MB
#define BELLATRIX_LEGACY_Z2_RAM_MB 8u
#endif
    bellatrix_zorro2_enable_fast_ram((uint32_t)(BELLATRIX_LEGACY_Z2_RAM_MB) * 1024u * 1024u);
    kprintf("[BELA] legacy non-boards: Z2 RAM %uMB via Bellatrix autoconfig\n",
            (unsigned)(BELLATRIX_LEGACY_Z2_RAM_MB));
#endif

    bellatrix_machine_init(cpu_backend);
#ifdef BELLATRIX_CORE_LOG
    kprintf("[BUILD] BELLATRIX_CORE_LOG: ON\n");
#else
    kprintf("[BUILD] BELLATRIX_CORE_LOG: OFF\n");
#endif
    memset(&g_runtime, 0, sizeof(g_runtime));
    g_runtime.machine = bellatrix_machine_get();
    /* ISSUE-0036: switch kprintf from direct/blocking mode to the ring
     * buffer BEFORE core_io_init() (which brings up USB/DWC2) rather than
     * after it (previously done much later, alongside Paula's own serial
     * bridge open). USB init hammers the DWC2 controller with heavy
     * back-to-back MMIO/DMA -- confirmed on hardware that direct-mode
     * kprintf calls issued during that exact window corrupt the mini-UART
     * output (probably peripheral-bus contention), non-deterministically,
     * while everything printed once buffered/deferred (and drained later,
     * once the chipset step loop is calmly running) comes out clean. */
    console_log_set_deferred();

    /* core_io_init (not a bare usb_host_init) — it sets io.running, without
     * which core_io_step() is a silent no-op and USB dies after the launcher
     * (PAL_Runtime_Poll → bellatrix_runtime_io_step → early return). */
    core_io_init(&g_runtime.io, g_runtime.machine);
    bellatrix_console_log_reclock(400000000u);

    bellatrix_machine_attach_rom((const uint8_t *)ROM_KVIRT, BELLATRIX_ROM_SIZE);
    bellatrix_memory_set_overlay(bellatrix_machine_memory(), 1);

    /* Bellatrix-specific CIA-A defaults: OVL and LED are outputs */
    BellatrixMachine *m = bellatrix_machine_get();
    m->memory.chip_ram = (uint8_t *)CHIP_RAM_KVIRT;
    m->memory.chip_ram_size = BELLATRIX_CHIP_RAM_SIZE;
    m->memory.chip_ram_mask = BELLATRIX_CHIP_RAM_MASK;
    /* Clear chip RAM so M68K sees a clean slate (no ARM boot residue).
     * Without this, (4).W (SysBase) contains ARM instruction bytes which
     * confuses the Kickstart's early VBL handler into computing a wrong
     * exec dispatch address and installing a bad vector at 0x6c. */
    memset(m->memory.chip_ram, 0, m->memory.chip_ram_size);
    m->memory.fast_ram = (uint8_t *)FAST_RAM_KVIRT;
    m->memory.fast_ram_size = BELLATRIX_FAST_RAM_SIZE;
    m->memory.fast_ram_mask = BELLATRIX_FAST_RAM_MASK;
    memset(m->memory.fast_ram, 0, m->memory.fast_ram_size);

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

        /* Detect 1 MB ROM: start.c copies first 512 KB to physical 0xe00000
         * and second 512 KB to 0xf80000.  For a plain 512 KB Kickstart it
         * mirrors the same data to both addresses, so they're identical.  If
         * the two halves differ, we have a 1 MB ROM (e.g. AROS) and must
         * expose the extended window to the Musashi CPU backend. */
        if (rom_mapped)
        {
            const uint8_t *ext = (const uint8_t *)ROM_EXT_KVIRT;
            const uint8_t *std = (const uint8_t *)ROM_KVIRT;
            int is_1mb = 0;
            for (int i = 0; i < 8; i++) {
                if (ext[i] != std[i]) { is_1mb = 1; break; }
            }
            if (is_1mb) {
                bellatrix_memory_attach_ext_rom(&m->memory, ext,
                                               BELLATRIX_EXT_ROM_SIZE);
                kprintf("[BELA] 1MB ROM: extended module ROM attached"
                        " @ 0xe00000 (%u KB)\n",
                        (unsigned)(BELLATRIX_EXT_ROM_SIZE / 1024u));
            }
        }
    }

    /* Chip RAM: configured visible window.
     * Legacy non-boards path extends the mapped region to 2MB so that
     * 0x100000-0x1FFFFF is accessible to the CPU as "Slow RAM" (not DMA-accessible). This
     * matches the original Bellatrix approach and gives the OS more room,
     * which is why the boot progressed further.  Z2 fast RAM at 0x200000+
     * does not overlap with this range. */
#if BELLATRIX_ENABLE_EMU68_BOARDS
    mmu_map(0x000000, 0x000000, BELLATRIX_CHIP_RAM_SIZE,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
#else
    mmu_map(0x000000, 0x000000, 0x00200000u,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
    kprintf("[BELA] legacy non-boards MMU: chip+slow RAM 0x000000-0x1FFFFF (2MB)\n");
#endif

    /* Install the low-memory window according to the initial OVL state. */
    apply_overlay_map(1);
    s_overlay = 1;

    /* CIA-B ($BFD000) and CIA-A ($BFE000) belong to the Bellatrix chipset.
     * Leave them fault-driven so the Emu68 CPU path hands those accesses to
     * Bellatrix instead of satisfying them from the generic 1:1 map. */
    mmu_map(0xBFD000, 0xBFD000, 0x1000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
    mmu_map(0xBFE000, 0xBFE000, 0x1000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    mmu_map(0xC00000, 0xC00000, 0x200000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);
    mmu_map(0xF00000, 0xF00000, 0x80000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_READ_ONLY | MMU_ATTR_CACHED, 0);

#if BELLATRIX_ENABLE_EMU68_BOARDS
    /* AutoConfig belongs to Emu68 boards on the real target. Force the
     * 0x00e80000 window through the vectors path so the native board handler
     * can expose board ROMs and complete assignment without Bellatrix owning
     * the config aperture. */
    mmu_map(0x00E80000u, 0x00E80000u, 0x00010000u,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
#else
    /* Legacy non-boards path: Bellatrix handles Zorro II config via its own memory map.
     * Fault-drive the config window so every read/write reaches
     * bellatrix_bus_access → memory_map → zorro2_bus.c. */
    mmu_map(0x00E80000u, 0x00E80000u, 0x00010000u,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    /* Pre-map the full Z2 Fast RAM range with full R/W access.
     * The JIT can access it directly after the OS assigns the base via
     * autoconfig.  We always map at BELLATRIX_FAST_RAM_BASE (0x200000)
     * because Kickstart/AROS assigns the first Z2 board there. */
    mmu_map(BELLATRIX_FAST_RAM_BASE,
            BELLATRIX_FAST_RAM_BASE,
            (uint32_t)(BELLATRIX_LEGACY_Z2_RAM_MB) * 1024u * 1024u,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
    kprintf("[BELA] legacy non-boards MMU: Z2 Fast RAM %08x-%08x mapped\n",
            (unsigned)BELLATRIX_FAST_RAM_BASE,
            (unsigned)(BELLATRIX_FAST_RAM_BASE +
                       (uint32_t)(BELLATRIX_LEGACY_Z2_RAM_MB) * 1024u * 1024u - 1u));
#endif

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

#if defined(BELLATRIX_UART_LOG)
    kprintf("[SERIAL] log mode — Paula TX forwarded to kprintf [SERIAL] prefix; no UART bridge\n");
#else
    if (uart_host_open_pty(&m->uart_host))
    {
        const char *pty_name = uart_host_pty_name(&m->uart_host);
        if (pty_name)
        {
            kprintf("[SERIAL] PTY ready: %s\n", pty_name);
        }
    }
#if BELLATRIX_ENABLE_BTSTACK
    else
    {
        kprintf("[SERIAL] BTStack owns on-board UART path; Paula host serial bridge disabled\n");
    }
#else
    /* Must match bellatrix_console_log_init_early(): both paths touch the
     * same physical AUX mini-UART, and the last open wins the baud register. */
    else if (uart_host_open_miniuart_clk(&m->uart_host, 115200, 400000000u))
    {
#if defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 1)
        uart_host_set_null_modem_mode(&m->uart_host, NULL_MODEM_LOOPBACK);
#elif defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 2)
        uart_host_set_null_modem_mode(&m->uart_host, NULL_MODEM_LOOPBACK_ONESHOT);
#endif
        uint32_t lsr = miniuart_backend_read_lsr();
        kprintf("[SERIAL] mini-UART open at 115200 baud  LSR=0x%08x TX_ready=%s\n",
                lsr, (lsr & 0x20u) ? "yes" : "no (QEMU AUX UART may be unresponsive)");
        console_log_set_deferred();
#if defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 1)
        kprintf("[SERIAL] internal serial loopback enabled\n");
#elif defined(BELLATRIX_UART_LOOPBACK_MODE) && (BELLATRIX_UART_LOOPBACK_MODE == 2)
        kprintf("[SERIAL] internal serial probe loopback enabled\n");
#endif
    }
#endif
#endif

#if BELLATRIX_ENABLE_BTSTACK
    bellatrix_init_bluetooth(&g_runtime, m);
#endif

#if BELLATRIX_ENABLE_HDMI_AUDIO
    hdmi_audio_init();
#endif

#ifdef BELLATRIX_ENABLE_MULTICORE
    /* Enable secondary chipset cores only after host-side services are ready. */
    PAL_Core_SetMulticoreEnabled(1);
    PAL_Core_LaunchChipset(NULL);   /* Core 1 — chipset */
    PAL_Core_LaunchIO();            /* Core 3 — physical IO */
#else
    /*
     * Keep Bellatrix in single-core mode so Emu68's normal bootstrap/JIT flow
     * remains the only scheduler path active.
     */
    PAL_Core_SetMulticoreEnabled(0);
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

#if BELLATRIX_ENABLE_BTSTACK
    if (g_runtime.bluetooth.initialized) {
        if (!bt_host_wait_for_bootstrap(&g_runtime.bluetooth, 20000u)) {
            kprintf("[BT] bootstrap window ended without WORKING; continuing boot\n");
        } else {
            kprintf("[BT] bootstrap completed before releasing boot\n");
        }
    }
#endif

#ifdef BELLATRIX_LAUNCHER
    launcher_run();
#endif

#if BELLATRIX_ENABLE_BTSTACK
    /* bt_pairs is populated by launcher_run() (reads BTPAIRS.TXT from SD).
     * Connect to saved HID devices now that the list is available.
     * Pump for 5 s: paging + SDP + L2CAP + HID SET_PROTOCOL take 3-6 s. */
    bt_host_connect_pairs(&g_runtime.bluetooth);
    if (g_runtime.bluetooth.initialized) {
        uint32_t _t0 = hal_time_ms();
        while ((hal_time_ms() - _t0) < 5000u)
            bt_host_step(&g_runtime.bluetooth);
    }
#ifdef BELLATRIX_LAUNCHER
    /* Write BTSCAN.TXT again — this time it captures the connect_pairs
     * log entries and any HID connection events from the pump above. */
    launcher_save_bt_report();
#endif
#endif

    core_chipset_init(&g_runtime.chipset,
                      bellatrix_machine_rigel_ctx(),
                      g_runtime.machine);

    kprintf("[BELA] build: " __DATE__ " " __TIME__ "\n");
    if (PAL_Core_IsMulticoreEnabled()) {
        kprintf("[BELA] Initialized (multicore enabled: Core1=CPU Core2=Chipset Core3=IO)\n");
    } else {
        kprintf("[BELA] Initialized (single-core mode: Core0 runs CPU+Chipset+IO)\n");
    }

#if defined(BELLATRIX_USE_MUSASHI_CPU) && BELLATRIX_USE_MUSASHI_CPU
    kprintf("[BELA] CPU backend: musashi\n");
#else
    kprintf("[BELA] CPU backend: emu68\n");
#endif
#if BELLATRIX_PROFILE_ENABLED
    bellatrix_profile_reset();
    kprintf("[BELA] MMIO profiling: ENABLED (BELLATRIX_PROFILE=1)\n");
#else
    kprintf("[BELA] MMIO profiling: disabled\n");
#endif
}

#ifdef BELLATRIX_LAUNCHER
void bellatrix_launcher_pump_usb(void)
{
    usb_host_step(&g_runtime.io.usb_host);
}

/* BT pump + readiness for the launcher's scan/pairing screen.  Defined even
 * without BTSTACK so launcher.c links unconditionally. */
void bellatrix_launcher_pump_bt(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    bt_host_step(&g_runtime.bluetooth);
#endif
}

int bellatrix_launcher_bt_ready(void)
{
#if BELLATRIX_ENABLE_BTSTACK
    return bt_host_is_working(&g_runtime.bluetooth) ? 1 : 0;
#else
    return 0;
#endif
}
#endif

void bellatrix_sync_overlay_from_ciaa(void)
{
    struct RigelContext *ctx = bellatrix_machine_rigel_ctx();
    uint8_t pra  = ctx ? (uint8_t)rigel_cia_read(ctx, 0u, 0x0u) : 0u;
    uint8_t ddra = ctx ? (uint8_t)rigel_cia_read(ctx, 0u, 0x2u) : 0u;
    int new_ovl = (int)(pra & 1u);

    if (new_ovl != s_overlay)
    {
        kprintf("[OVL-TRIG-LIVE] pra=%02x ddra=%02x new_ovl=%d fault_pc=%08x\n",
                (unsigned)pra,
                (unsigned)ddra,
                new_ovl,
                (unsigned)g_bellatrix_fault_pc);
    }

    set_overlay(new_ovl);
}

/* ---------------------------------------------------------------------------
 * Bus dispatch
 * ------------------------------------------------------------------------- */

uint32_t bellatrix_bus_access(uint32_t addr, uint32_t value, int size, int dir)
{
#if BELLATRIX_PROFILE_ENABLED
    uint64_t _t0 = bprof_now();
    uint64_t _t1 = _t0;
    bellatrix_runtime_poll_from_emu68();
    bprof_record(&g_bprof.poll, bprof_now() - _t1);
#else
    bellatrix_runtime_poll_from_emu68();
#endif

    uint32_t result = 0;

#if BELLATRIX_PROFILE_ENABLED
    {
        uint64_t _ta = bprof_now();
        if (!bellatrix_slow_contains(bellatrix_machine_memory(), addr, (unsigned int)size))
            addr = bellatrix_bridge_normalize_addr(addr);
        bprof_record(&g_bprof.addr_fix, bprof_now() - _ta);
    }
    /* Region classification (after normalization) */
    {
        BellatrixProfileBucket *_rbkt;
        if      (addr >= 0xBFE000u && addr <= 0xBFEFFFu) _rbkt = &g_bprof.region_cia_a;
        else if (addr >= 0xBFD000u && addr <= 0xBFDFFFu) _rbkt = &g_bprof.region_cia_b;
        else if (addr >= 0xDFF09Au && addr <= 0xDFF09Du) _rbkt = &g_bprof.region_ocs_intr;
        else if (addr >= 0xDFF000u && addr <= 0xDFF1FFu) _rbkt = &g_bprof.region_ocs_other;
        else                                              _rbkt = &g_bprof.region_other;
        _rbkt->calls++;
        bprof_hot_record(addr, g_bellatrix_fault_pc, dir);
    }
#else
    if (!bellatrix_slow_contains(bellatrix_machine_memory(), addr, (unsigned int)size))
        addr = bellatrix_bridge_normalize_addr(addr);
#endif

#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
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

    {
        uint32_t real_pc = g_bellatrix_fault_pc;

        if (real_pc != 0u && bellatrix_chip_addr_contains(real_pc))
        {
            kprintf("[PC-CHIPMEM] fault_pc=%08x addr=%06x %s size=%d\n",
                    (unsigned)real_pc, (unsigned)addr,
                    dir == BUS_READ ? "R" : "W", size);
        }

        if (dir == BUS_WRITE && real_pc >= 0xfc5e00u && real_pc <= 0xfc5fffu)
        {
            kprintf("[PC-TRAP] pc=%08x addr=%06x %s size=%d val=%08x\n",
                    (unsigned)real_pc, (unsigned)addr,
                    dir == BUS_READ ? "R" : "W", size, (unsigned)value);
        }
    }
#endif

    /* Control addresses — not counted in profiling totals */
    if (addr == BTRACE_CONTROL_ADDR && dir == BUS_WRITE)
    {
        bellatrix_machine_btrace_set_filter((uint16_t)value);
        return 0;
    }
#if BELLATRIX_PROFILE_ENABLED
    if (addr == BPROF_CONTROL_ADDR && dir == BUS_WRITE)
    {
        if (value == 0x01u) bellatrix_profile_dump();
        if (value == 0x02u) bellatrix_profile_reset();
        return 0;
    }
#endif

    if (dir == BUS_WRITE)
    {
#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
        if (addr < 0x400u)
        {
            kprintf("[VEC-W] %05x[%d]=%08x\n",
                    (unsigned)addr, size, (unsigned)value);
        }
        else if (addr >= 0x1000u && addr < 0x2000u)
        {
            kprintf("[JMP-W] %05x[%d]=%08x\n",
                    (unsigned)addr, size, (unsigned)value);
        }
        else if (addr >= 0x02368u && addr < 0x02420u)
        {
            kprintf("[CHIPRAM-W] addr=%05x size=%d value=%08x\n",
                    (unsigned)addr, size, (unsigned)value);
        }
#endif

        /*
         * core_chipset_lock_acquire/release() (held inside
         * bellatrix_bridge_cpu_write itself) prevents concurrent
         * rigel_step() on Core 2 while Core 1 writes.
         */
#if BELLATRIX_PROFILE_ENABLED
        { uint64_t _td = bprof_now(); bellatrix_bridge_cpu_write(addr, value, (unsigned)size); bprof_record(&g_bprof.dispatch_write, bprof_now() - _td); }
        g_bprof.writes++;
#else
        bellatrix_bridge_cpu_write(addr, value, (unsigned)size);
#endif

        /*
         * CIA-A PRA bit 0 controls the host MMU overlay mapping.
         * The logical CIA state was already updated by bellatrix_machine_write().
         * This reads Rigel directly (bypassing the bridge), so it needs its
         * own lock around the read.
         */
        if (addr >= 0xBFE001u &&
            addr <= 0xBFEF01u &&
            (addr & 0xFFu) == 0x01u &&
            cia_reg(addr) == 0)
        {
            struct RigelContext *ctx = bellatrix_machine_rigel_ctx();
            core_chipset_lock_acquire();
            uint8_t pra = ctx ? (uint8_t)rigel_cia_read(ctx, 0u, 0x0u) : 0u;
            core_chipset_lock_release();
            int new_ovl = (int)(pra & 1u);

#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
            if (new_ovl != s_overlay)
            {
                kprintf("[OVL-TRIG] ciaa_pra_write addr=%08x val=%02x pra=%02x new_ovl=%d\n",
                        (unsigned)addr,
                        (unsigned)(value & 0xFFu),
                        (unsigned)pra,
                        new_ovl);
            }
#endif
            set_overlay(new_ovl);
        }

#if BELLATRIX_PROFILE_ENABLED
        bprof_record(&g_bprof.total_access, bprof_now() - _t0);
        if (g_bprof.total_access.calls % BPROF_AUTODUMP_INTERVAL == 0)
            bellatrix_profile_dump();
#endif
        return 0;
    }

#if BELLATRIX_PROFILE_ENABLED
    { uint64_t _td = bprof_now(); result = bellatrix_bridge_cpu_read(addr, (unsigned)size); bprof_record(&g_bprof.dispatch_read, bprof_now() - _td); }
    g_bprof.reads++;
    bprof_record(&g_bprof.total_access, bprof_now() - _t0);
    if (g_bprof.total_access.calls % BPROF_AUTODUMP_INTERVAL == 0)
        bellatrix_profile_dump();
#else
    result = bellatrix_bridge_cpu_read(addr, (unsigned)size);
#endif
#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
    {
        uint32_t real_pc = g_bellatrix_fault_pc;
        if (real_pc >= 0xfc5e00u && real_pc <= 0xfc5fffu)
        {
            kprintf("[PC-TRAP] pc=%08x addr=%06x R size=%d result=%08x\n",
                    (unsigned)real_pc, (unsigned)addr, size,
                    (unsigned)result);
        }
    }
#endif
    return result;
}

/* ---------------------------------------------------------------------------
 * CPU step hook — drives machine timing
 * ------------------------------------------------------------------------- */

void bellatrix_cpu_step(uint32_t cycles)
{
    bellatrix_bridge_publish_cpu_cycles(cycles);
}
