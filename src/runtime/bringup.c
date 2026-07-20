/*
 * Machine bring-up: the ordered boot sequence that turns a bare Pi into a
 * running Amiga.
 *
 * Split out of src/cpu/emu68/bellatrix.c (2026-07-20). It had accumulated
 * there because the Emu68 adapter was the first thing to exist, with the
 * consequence that the Musashi build reached its own bring-up *through* the
 * Emu68 adapter file.
 *
 * Two phases stayed behind, and deliberately so: attaching ROM/RAM and
 * laying out guest memory both work in Emu68 kernel virtual space
 * (0xffffff90_00000000...) and are Emu68's to own. They are reached through
 * bellatrix.h. That is the real shape of the boundary — the boot sequence
 * interleaves neutral and backend-specific work rather than separating into
 * two contiguous halves, and the ownership is now explicit at each call.
 *
 * Today there is exactly one implementation of those two, which is why the
 * Musashi build also runs on Emu68's memory topology.
 */

#include "cpu/emu68/bellatrix.h"

#include "cpu/cpu_backend.h"
#include "machine/machine.h"
#include "machine/expansions/z2_fast_ram/z2_fast_ram.h"
#include "runtime/runtime.h"
#include "runtime/core_io.h"
#include "runtime/core_chipset.h"
#include "runtime/topology.h"
#include "host/pal.h"
#include "host/raspi3/console_log.h"
#include "host/raspi3/hdmi_audio.h"
#include "io/serial/uart_host.h"
#include "io/serial/null_modem.h"
#include "io/serial/miniuart_backend.h"
#include "cpu/emu68/bellatrix_profile.h"
#ifdef BELLATRIX_LAUNCHER
#include "launcher/launcher.h"
#include "launcher/btscan.h"
#endif
#include "devicetree.h"
#include "support.h"

#include <stdatomic.h>
#include <string.h>


/* Boot-time timeline selection: the build default (BELLATRIX_TIMELINE_MODE)
 * can be overridden per boot with `timeline=cpu|realtime|hybrid` in the
 * kernel bootargs (cmdline.txt on SD, BOOTARGS/-append in QEMU) — the Fase 0
 * requirement of A/B without recompiling. Same /chosen pattern as Emu68's
 * async_log. */
/* Where the selected mode came from — printed with the mode so an A/B run
 * can always tell "cmdline parsed and accepted" from "cmdline never seen",
 * even when the requested mode equals the build default. */
static const char *s_timeline_mode_source = "build default (no bootargs)";

static RuntimeTimelineMode bellatrix_timeline_boot_mode(RuntimeTimelineMode fallback)
{
    of_node_t *chosen = dt_find_node("/chosen");
    of_property_t *prop;
    const char *args;
    const char *opt;

    if (!chosen)
        return fallback;
    prop = dt_find_property(chosen, "bootargs");
    if (!prop || !prop->op_value)
        return fallback;

    args = prop->op_value;
    opt = strstr(args, "timeline=");
    if (!opt) {
        s_timeline_mode_source = "build default (no timeline= in bootargs)";
        return fallback;
    }
    opt += 9;

    if (strncmp(opt, "cpu", 3) == 0) {
        s_timeline_mode_source = "bootargs";
        return RUNTIME_TIMELINE_CPU_DRIVEN;
    }
    if (strncmp(opt, "realtime", 8) == 0) {
        s_timeline_mode_source = "bootargs";
        return RUNTIME_TIMELINE_REALTIME;
    }
    if (strncmp(opt, "hybrid", 6) == 0) {
        s_timeline_mode_source = "bootargs";
        return RUNTIME_TIMELINE_HYBRID;
    }

    s_timeline_mode_source = "build default (timeline= value not recognized)";
    return fallback;
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

/* Phase 1a — serial up, and on multicore prove how many PEs entered here. */
static void bringup_report_entry(void)
{
#if defined(BELLATRIX_ENABLE_MULTICORE)
    static _Atomic uint32_t s_init_entry_count;
    uint64_t init_mpidr;
    uintptr_t init_lr;
    uintptr_t init_sp;
#endif
    PAL_Debug_Init(115200);
#if defined(BELLATRIX_ENABLE_MULTICORE)
    asm volatile("mrs %0, MPIDR_EL1" : "=r"(init_mpidr));
    asm volatile("mov %0, x30" : "=r"(init_lr));
    asm volatile("mov %0, sp" : "=r"(init_sp));
    kprintf("[BELA-INIT-ENTRY] count=%u core=%u lr=%016llx sp=%016llx\n",
            (unsigned)(atomic_fetch_add_explicit(&s_init_entry_count, 1u,
                                                 memory_order_relaxed) + 1u),
            (unsigned)(init_mpidr & 0xffu),
            (unsigned long long)init_lr,
            (unsigned long long)init_sp);
#endif
}

/* Phase 1b — choose the CPU backend and build the machine around it. */
static void bringup_select_cpu_and_machine(void)
{
    CpuBackend *cpu_backend;

    bellatrix_emu68_boards_reset();
    cpu_backend = cpu_backend_selected();
    cpu_backend_init_selected();

    bellatrix_machine_init(cpu_backend);
#if !BELLATRIX_ENABLE_EMU68_BOARDS
    /* Bellatrix owns the Z2 protocol in the non-native-boards path. Register
     * the board now, but map no guest RAM: its map() callback is invoked only
     * when the guest assigns a base through Autoconfig. */
#ifndef BELLATRIX_LEGACY_Z2_RAM_MB
#define BELLATRIX_LEGACY_Z2_RAM_MB 8u
#endif
    bellatrix_z2_fast_ram_configure(
        (uint32_t)BELLATRIX_LEGACY_Z2_RAM_MB * 1024u * 1024u);
    kprintf("[BELA] Z2 Fast RAM %uMB configured; awaiting guest base\n",
            (unsigned)BELLATRIX_LEGACY_Z2_RAM_MB);
#endif
#ifdef BELLATRIX_CORE_LOG
    kprintf("[BUILD] BELLATRIX_CORE_LOG: ON\n");
#else
    kprintf("[BUILD] BELLATRIX_CORE_LOG: OFF\n");
#endif
}

/* Phase 2 — runtime object, deferred console, host I/O reactor. */
static void bringup_host_services(void)
{
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

    /* Phase marker, flushed synchronously on Core 0 (Core 3 — the normal drainer
     * — is not launched yet, so there is no cross-core drain race, and USB init
     * has just finished so the bus is idle). If this is the LAST line seen on
     * real hardware, the stall is inside core_io_init()/usb_host_init(), not
     * later; the deferred console would otherwise hide exactly where it stops. */
    kprintf("[PHASE] host services up\n");
    console_log_drain();
}

/* Phase 5 — PAL runtime, Paula serial backend, Bluetooth, HDMI audio. */
static void bringup_host_io(void)
{
    BellatrixMachine *m = bellatrix_machine_get();

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
}

/* Phase 6 — enable secondary cores, then run the launcher.
 * Core 2 is deliberately NOT started here; see bringup_start_runtime(). */
static void bringup_launcher_phase(void)
{
#ifdef BELLATRIX_ENABLE_MULTICORE
    /* Enable secondary chipset cores only after host-side services are ready. */
    PAL_Core_SetMulticoreEnabled(1);
    /* Core 0 owns physical IO during the launcher. Core 3 owns the same
     * reactor after CPU launch; only bounded IRQ top halves remain on Core 0.
     *  - Core 2 (chipset) is deferred until after the launcher + chipset init
     *    (see below): with no M68K running yet it has no work during the
     *    launcher, and letting it run there raced shared state on real hardware. */
#else
    /*
     * Keep Bellatrix in single-core mode so Emu68's normal bootstrap/JIT flow
     * remains the only scheduler path active.
     */
    PAL_Core_SetMulticoreEnabled(0);
#endif

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

    /* Preserve boot-time host-I/O costs separately: enumeration/MSC may legitimately
     * block before CPU/chipset launch and must not contaminate runtime maxima. */
    {
        CoreIOReactorStats launcher_io;
        uint64_t freq = PAL_Time_GetFrequency();
        core_io_reactor_get_stats(&g_runtime.io, &launcher_io);
        kprintf("[HOST-IO-BOOT] calls=%llu budget_miss=%u max=%lluus "
                "late_max=%lluus usb=%lluus\n",
                (unsigned long long)launcher_io.dispatch_calls,
                (unsigned)launcher_io.over_budget,
                (unsigned long long)(launcher_io.max_ticks * 1000000u / freq),
                (unsigned long long)(launcher_io.max_late_ticks * 1000000u / freq),
                (unsigned long long)(launcher_io.usb_max_ticks * 1000000u / freq));
        core_io_reactor_reset_stats(&g_runtime.io);
    }

    /* Launcher done; the same reactor continues from the supervisor loop. */

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
}

/* Phase 7 — chipset context, timeline, worker cores, boot banners. */
static void bringup_start_runtime(void)
{
    core_chipset_init(&g_runtime.chipset,
                      bellatrix_machine_rigel_ctx(),
                      g_runtime.machine);

    /* Initialise the machine timeline before any worker starts. The active
     * host reactor updates it later, regardless of its numbered core. Keeping
     * this outside either core-specific loop makes
     * STOP/IRQ progress and presentation independent of CPU placement. */
    {
#ifndef BELLATRIX_TIMELINE_DEFAULT
#define BELLATRIX_TIMELINE_DEFAULT 0
#endif
        uint64_t now = PAL_Time_ReadCounter();
        uint64_t freq = PAL_Time_GetFrequency();
        RuntimeTimelineMode mode = bellatrix_timeline_boot_mode(
            (RuntimeTimelineMode)BELLATRIX_TIMELINE_DEFAULT);
        kprintf("[HOST] timeline mode: %s (%s)\n",
                mode == RUNTIME_TIMELINE_CPU_DRIVEN ? "cpu-driven" :
                mode == RUNTIME_TIMELINE_REALTIME   ? "realtime" : "hybrid",
                s_timeline_mode_source);
        core_chipset_timeline_init(now, freq, mode);
    }

#ifdef BELLATRIX_ENABLE_MULTICORE
    /* Runtime phase begins: the launcher is done and the chipset context is
     * initialised, so bring up Core 2 (chipset) now. Deferring it to here (from
     * before the launcher) keeps the launcher phase free of a second core
     * touching shared state (ISSUE-0042/0044). */
    PAL_Core_LaunchChipset(NULL);   /* Core 2 — chipset */
    /* Core 0 enters the selected CPU loop; Core 3 owns the common reactor. */
    PAL_Core_LaunchHostReactor();
#endif

    kprintf("[BELA] build: " __DATE__ " " __TIME__ "\n");
    if (PAL_Core_IsMulticoreEnabled()) {
        kprintf("[BELA] topology: boot=%u cpu=%u chipset=%u host=%u aux=%u%s\n",
                (unsigned)BELLATRIX_CORE_BOOT,
                (unsigned)BELLATRIX_CORE_CPU,
                (unsigned)BELLATRIX_CORE_CHIPSET,
                (unsigned)BELLATRIX_CORE_HOST_REACTOR,
                (unsigned)BELLATRIX_CORE_AUXILIARY,
                " (unified CPU placement)");
    } else {
        kprintf("[BELA] Initialized (single-core mode: Core0 runs CPU+Chipset+IO)\n");
    }

    cpu_backend_log_selected();
#if BELLATRIX_PROFILE_ENABLED
    bellatrix_profile_reset();
    kprintf("[BELA] MMIO profiling: ENABLED (BELLATRIX_PROFILE=1)\n");
#else
    kprintf("[BELA] MMIO profiling: disabled\n");
#endif
#if defined(BELLATRIX_COARSE_OBSERVABLE_DEADLINES) && \
    BELLATRIX_COARSE_OBSERVABLE_DEADLINES
    kprintf("[BELA] EXPERIMENTAL coarse observable deadlines: ENABLED\n");
#else
    kprintf("[BELA] coarse observable deadlines: disabled\n");
#endif
}

void bellatrix_init(void)
{
    bringup_report_entry();
    bringup_select_cpu_and_machine();
    bringup_host_services();
    bellatrix_emu68_attach_rom_and_ram();
    bellatrix_emu68_map_guest_memory();
    bringup_host_io();
    bringup_launcher_phase();
    bringup_start_runtime();
}
