// src/cpu/emu68/bellatrix.c
//
// Emu68 integration entry point for the Bellatrix chipset emulator.
// Routes every unmapped M68K bus access to the appropriate chipset module.

#include "cpu/emu68/bellatrix.h"
#include "cpu/emu68/emu68_backend.h"
#include "cpu/emu68/bellatrix_profile.h"
#include "cpu/cpu_bridge.h"
#include "runtime/runtime.h"
#ifdef BELLATRIX_LAUNCHER
#include "launcher/launcher.h"
#endif
#include <stdatomic.h>
#include <limits.h>
#include "cpu/cpu_backend.h"
#include "machine/machine.h"
#include "rigel/rigel_custom.h"
#include "rigel/rigel_cia.h"
#include "rigel/rigel_irq.h"
#include "rigel/rigel_mmio.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "debug/cpu_pc.h"
#include "host/pal.h"
#include "host/raspi3/hdmi_audio.h"
#include "host/raspi3/vc_mailbox.h"
#include "host/osd.h"
#include "audio/output.h"
#include "devicetree.h"
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

#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
/* IRQ delivery observation ring, written by the MainLoop delivery block
 * (plain stores, no calls). Entry: pushed PC (low 24 bits) | level << 28. */
uint32_t g_bela_irq_deliver_count;
uint32_t g_bela_irq_deliver_ring[8];
#endif

extern struct M68KState *__m68k_state;
/* This file owns the Bellatrix environment and the bus implementation offered
 * to CPU backends. Emu68 CPU ownership lives in emu68_backend.c. */

void bellatrix_machine_advance_cpu_cycles(uint32_t cycles);

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
#if defined(BELLATRIX_EMU68_FAULT_DRIVEN) && BELLATRIX_EMU68_FAULT_DRIVEN
    /* The conservative path enters M68K_StartEmu directly. The public machine
     * driver remains compiled only as an A/B rollback option. */
    return 0;
#else
    return cpu_backend_owns_execution_loop();
#endif
}

void bellatrix_run_selected_cpu_backend(void)
{
    cpu_backend_run_selected();
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
/* Core 0 supervisor — replaces the idle wfe park in multicore builds.
 *
 * Debug scaffold and first increment of the Core-0 arbiter: instead of sleeping
 * forever after launching Core 1, Core 0 periodically reports whether the CPU
 * (Core 1) is publishing cycles (cpu_target) and the chipset (Core 2) is
 * draining them (chipset), so a silent-but-running boot can be told apart from a
 * stalled/deadlocked one. It does not schedule anything yet — see
 * AI_context/consolidated/issue_core0_arbiter_scheduler.md. */
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

static void bellatrix_core0_supervise(void)
{
    extern bool core_chipset_get_progress(uint64_t *chipset_cck,
                                          uint64_t *target_cck);
    uint64_t last_target = 0u;
    uint64_t last_chipset = 0u;
#if BELLATRIX_PROFILE_ENABLED
    uint64_t last_steps = 0u;
    uint64_t last_step_cck = 0u;
    uint64_t last_step_busy = 0u;
#endif
    const uint64_t freq = PAL_Time_GetFrequency();
    const uint64_t io_interval = freq / 1000u ? freq / 1000u : 1u;
    const uint64_t heartbeat_interval = freq * 2u;
    uint64_t last_io = PAL_Time_ReadCounter();
    uint64_t last_heartbeat = last_io;
    uint32_t beat = 0u;
    BellatrixAudioOutputStats last_audio = {0};
    RuntimeFrameCompletionStats last_frame_stats = {0};

#ifndef BELLATRIX_TIMELINE_DEFAULT
#define BELLATRIX_TIMELINE_DEFAULT 0
#endif
    RuntimeTimelineMode timeline_mode = bellatrix_timeline_boot_mode(
        (RuntimeTimelineMode)BELLATRIX_TIMELINE_DEFAULT);

    kprintf("[CORE0] timeline mode: %s (%s)\n",
            timeline_mode == RUNTIME_TIMELINE_CPU_DRIVEN ? "cpu-driven" :
            timeline_mode == RUNTIME_TIMELINE_REALTIME   ? "realtime" :
                                                           "hybrid",
            s_timeline_mode_source);

    core_chipset_timeline_init(last_io, freq, timeline_mode);

    for (;;) {
        uint64_t now = PAL_Time_ReadCounter();

        /* Keep physical IO bounded and poll at roughly 1 kHz. */
        if (now - last_io >= io_interval) {
            last_io = now;
            bellatrix_runtime_io_step(now, freq);
            (void)core_chipset_timeline_update(now);
            core_chipset_drain_host_completions();
        }

        if (now - last_heartbeat < heartbeat_interval) {
            asm volatile("yield");
            continue;
        }
        uint64_t beat_elapsed = now - last_heartbeat;
        last_heartbeat = now;

        uint64_t chipset_cck = 0u;
        uint64_t target_cck = 0u;
        uint64_t horizon_cck = core_chipset_get_horizon();
        RuntimeTimeline timeline;
        BellatrixMachine *machine = bellatrix_machine_get();
        CpuBackend *backend = cpu_backend_selected();
        uint64_t frames = machine
            ? __atomic_load_n(&machine->frame_counter, __ATOMIC_ACQUIRE)
            : 0u;
        uint32_t pc = backend ? cpu_backend_get_pc(backend) : 0u;
        CoreIOSerialStats serial_stats;
        CoreIOReactorStats io_stats;
        RuntimeFrameCompletionStats frame_stats;
        core_io_serial_get_stats(&serial_stats);
        core_io_reactor_get_stats(&g_runtime.io, &io_stats);
        (void)core_chipset_get_progress(&chipset_cck, &target_cck);
        core_chipset_get_timeline_snapshot(&timeline);
        core_chipset_get_frame_completion_stats(&frame_stats);

        {
            uint64_t expected_cck = beat_elapsed && freq
                ? beat_elapsed * timeline.cck_per_second / freq : 0u;
            uint64_t advanced_cck = chipset_cck - last_chipset;
            uint32_t realtime_pct = expected_cck
                ? (uint32_t)(advanced_cck * 100u / expected_cck) : 0u;
            osd_set_realtime_percent(realtime_pct);
        }

        kprintf("[CORE0-SUP] beat=%u mode=%u cpu_target=%llu(+%llu) "
                "horizon=%llu chipset=%llu(+%llu) backlog=%lld frames=%llu pc=%08x "
                "uart_tx=%u/%u drop=%u uart_rx=%u/%u drop=%u\n",
                (unsigned)beat,
                (unsigned)core_chipset_timeline_mode(),
                (unsigned long long)target_cck,
                (unsigned long long)(target_cck - last_target),
                (unsigned long long)horizon_cck,
                (unsigned long long)chipset_cck,
                (unsigned long long)(chipset_cck - last_chipset),
                (long long)((int64_t)target_cck - (int64_t)chipset_cck),
                (unsigned long long)frames, (unsigned)pc,
                (unsigned)serial_stats.tx_depth,
                (unsigned)serial_stats.tx_max_depth,
                (unsigned)serial_stats.tx_dropped,
                (unsigned)serial_stats.rx_depth,
                (unsigned)serial_stats.rx_max_depth,
                (unsigned)serial_stats.rx_dropped);
        kprintf("[CORE0-TIME] realtime=%llu horizon=%llu rigel=%llu drift=%lld "
                "clamp=%u generation=%llu\n",
                (unsigned long long)timeline.realtime_target_cck,
                (unsigned long long)timeline.horizon_cck,
                (unsigned long long)chipset_cck,
                (long long)((int64_t)timeline.horizon_cck -
                            (int64_t)chipset_cck),
                timeline.clamp_active ? 1u : 0u,
                (unsigned long long)timeline.generation);
        kprintf("[CORE0-FRAME] produced=%llu(+%llu) presented=%llu(+%llu) "
                "coalesced=%llu(+%llu)\n",
                (unsigned long long)frame_stats.produced,
                (unsigned long long)(frame_stats.produced -
                                     last_frame_stats.produced),
                (unsigned long long)frame_stats.presented,
                (unsigned long long)(frame_stats.presented -
                                     last_frame_stats.presented),
                (unsigned long long)frame_stats.coalesced,
                (unsigned long long)(frame_stats.coalesced -
                                     last_frame_stats.coalesced));
        last_frame_stats = frame_stats;

        /* Host silicon state: the firmware silently drops the ARM clock to
         * 600 MHz under undervoltage/thermal throttling, which halves every
         * performance number without any other visible symptom. Proven on
         * the real board: the second Pi 3 gate ran entirely throttled and
         * cost exactly 2x. Feed the OSD so the user sees UV! on screen. */
        {
            uint32_t throttled = vc_get_throttled();
            kprintf("[CORE0-HW] arm_mhz=%u throttled=%08x\n",
                    (unsigned)(vc_get_arm_clock_hz() / 1000000u),
                    (unsigned)throttled);
            osd_set_power_alert(throttled == 0xFFFFFFFFu ? 0u : throttled);
        }
#if BELLATRIX_PROFILE_ENABLED
        kprintf("[BPROF-BEAM] vposr_fast=%llu vposr_fallback=%llu "
                "vhposr_fast=%llu vhposr_fallback=%llu snapshot_miss=%llu\n",
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.beam_vposr_fast, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.beam_vposr_fallback, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.beam_vhposr_fast, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.beam_vhposr_fallback, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.beam_snapshot_miss, __ATOMIC_RELAXED));
        kprintf("[BPROF-POST] queued=%llu applied=%llu full_fallbacks=%llu depth_max=%llu\n",
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.posted_writes, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.posted_writes_applied, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.posted_queue_full_fallbacks, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.posted_queue_depth_max, __ATOMIC_RELAXED));
        /* Per-beat granularity and occupancy. avg_step is the decisive
         * integration metric (CCK advanced per rigel_step_until call): a
         * sub-scanline value means observable deadlines are slicing the
         * drain and any per-call fixed cost inside Rigel is multiplied.
         * core2_busy is wall time spent inside the locked step section. */
        uint64_t steps_now = __atomic_load_n(
            &g_bprof.multicore.chipset_steps, __ATOMIC_RELAXED);
        uint64_t step_cck_now = __atomic_load_n(
            &g_bprof.multicore.chipset_cck_total, __ATOMIC_RELAXED);
        uint64_t step_busy_now = __atomic_load_n(
            &g_bprof.chipset_step_time.cycles_total, __ATOMIC_RELAXED);
        uint64_t d_steps = steps_now - last_steps;
        uint64_t avg_step = d_steps
            ? (step_cck_now - last_step_cck) / d_steps : 0u;
        unsigned busy_pct = beat_elapsed
            ? (unsigned)((step_busy_now - last_step_busy) * 100u / beat_elapsed)
            : 0u;
        last_steps = steps_now;
        last_step_cck = step_cck_now;
        last_step_busy = step_busy_now;
        kprintf("[BPROF-SCHED] event_hz=%u publish=%llu explicit_wakeups=%llu "
                "chipset_steps=%llu avg_step=%llu core2_busy=%u%% "
                "empty_host_steps=%llu\n",
                (unsigned)PAL_Runtime_EventStreamHz(),
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.publish_calls, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.wakeups, __ATOMIC_RELAXED),
                (unsigned long long)steps_now,
                (unsigned long long)avg_step,
                busy_pct,
                (unsigned long long)__atomic_load_n(
                    &g_bprof.multicore.empty_host_steps, __ATOMIC_RELAXED));
#endif

        uint64_t io_avg_ticks = io_stats.dispatch_calls
            ? io_stats.total_ticks / io_stats.dispatch_calls : 0u;
        kprintf("[CORE0-IO] calls=%llu pending=%02x budget_miss=%u "
                "avg=%lluus max=%lluus late_max=%lluus "
                "usb=%lluus bt=%lluus serial=%lluus console=%lluus\n",
                (unsigned long long)io_stats.dispatch_calls,
                (unsigned)io_stats.pending_events,
                (unsigned)io_stats.over_budget,
                (unsigned long long)(io_avg_ticks * 1000000u / freq),
                (unsigned long long)(io_stats.max_ticks * 1000000u / freq),
                (unsigned long long)(io_stats.max_late_ticks * 1000000u / freq),
                (unsigned long long)(io_stats.usb_max_ticks * 1000000u / freq),
                (unsigned long long)(io_stats.bluetooth_max_ticks * 1000000u / freq),
                (unsigned long long)(io_stats.serial_max_ticks * 1000000u / freq),
                (unsigned long long)(io_stats.console_max_ticks * 1000000u / freq));

        /* Audio is meaningful only relative to wall time. Report deltas once
         * per heartbeat: a sub-realtime producer is immediately distinguishable
         * from an HDMI consumer/queue fault without per-sample trace noise. */
        {
            BellatrixAudioOutputStats audio = bellatrix_audio_output_stats();
            uint64_t expected = beat_elapsed
                ? beat_elapsed * BELLATRIX_AUDIO_OUTPUT_RATE_HZ / freq : 0u;
            uint64_t produced = audio.produced_samples - last_audio.produced_samples;
            uint64_t consumed = audio.consumed_samples - last_audio.consumed_samples;
            uint64_t dropped = audio.dropped_samples - last_audio.dropped_samples;
            uint64_t underrun = audio.underrun_samples - last_audio.underrun_samples;
            unsigned realtime_pct = expected
                ? (unsigned)(produced * 100u / expected) : 0u;

            kprintf("[CORE0-AUDIO] expected=%llu produced=%llu consumed=%llu "
                    "realtime=%u%% underrun=%llu dropped=%llu depth=%u "
                    "prime=%u primed=%u\n",
                    (unsigned long long)expected,
                    (unsigned long long)produced,
                    (unsigned long long)consumed,
                    realtime_pct,
                    (unsigned long long)underrun,
                    (unsigned long long)dropped,
                    (unsigned)bellatrix_audio_output_count(),
                    (unsigned)bellatrix_audio_output_prime_frames(),
                    bellatrix_audio_output_is_primed() ? 1u : 0u);
            last_audio = audio;
        }

        last_target = target_cck;
        last_chipset = chipset_cck;
        beat++;
    }
}

void bellatrix_launch_cpu_and_park(void (*entry)(void))
{
    if (!entry)
        return;

#if defined(BELLATRIX_EMU68_CORE0_REBASELINE) && \
    BELLATRIX_EMU68_CORE0_REBASELINE
    /* Preserve Emu68's native topology: the boot PE owns the JIT, VBAR,
     * TPIDRRO context and physical interrupt routing as one contract. The
     * selected entry does not return in fault-driven mode. */
    entry();
    return;
#endif

    if (!PAL_Core_IsMulticoreEnabled()) {
        entry();
        return;
    }

    PAL_Core_LaunchCpu(entry);

    bellatrix_core0_supervise();   /* never returns */
}

/* ---------------------------------------------------------------------------
 * Emu68/Musashi single-core CPU progress.
 *
 * This path advances the machine synchronously on the caller's core.  Do not
 * route it through cpu_bridge/runtime publish symbols; those are intentionally
 * owned by the generic runtime path and may resolve to the multicore publisher.
 * ------------------------------------------------------------------------- */
void bellatrix_machine_advance_cpu_cycles(uint32_t cycles)
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

void bellatrix_emu68_publish_cpu_progress(uint64_t cycles,
                                         uint64_t instructions,
                                         uint32_t pc)
{
    uint32_t publish_cycles = cycles > UINT32_MAX ?
        UINT32_MAX : (uint32_t)cycles;

    (void)instructions;
    g_bellatrix_exec_pc = pc;
    if (publish_cycles == 0u)
        return;
    if (PAL_Core_IsMulticoreEnabled())
        bellatrix_bridge_publish_cpu_cycles(publish_cycles);
    else
        bellatrix_machine_advance_cpu_cycles(publish_cycles);
}

void bellatrix_emu68_report_jit_progress(uint64_t insn_count, uint32_t pc)
{
    static uint64_t s_prev_cycle_count;
#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
    static uint32_t s_last_pc;
    static uint32_t s_same_pc_reports;
#endif
    uint32_t cycles = 0u;

    {
        uint64_t cycle_count = __m68k_state ? __m68k_state->CYCLE_COUNT : 0u;
        uint64_t delta;
        if (cycle_count < s_prev_cycle_count)
            s_prev_cycle_count = 0u;
        delta = cycle_count - s_prev_cycle_count;
        s_prev_cycle_count = cycle_count;
        cycles = delta > UINT32_MAX ? UINT32_MAX : (uint32_t)delta;
    }

    bellatrix_emu68_publish_cpu_progress(cycles, insn_count, pc);

    /* Safe liveness checkpoints: this function is called only while MainLoop
     * has spilled the pinned guest context. Keep diagnostics out of the live
     * JIT/IRQ delivery path. */
    {
        static unsigned checkpoint_index;
        static const uint32_t checkpoints[] = {
            100u, 500u, 1000u, 1500u, 2000u
        };
        BellatrixMachine *machine = bellatrix_machine_get();
        uint64_t frame = machine ? machine->frame_counter : 0u;
        if (checkpoint_index < (sizeof(checkpoints) / sizeof(checkpoints[0])) &&
            frame >= checkpoints[checkpoint_index]) {
            uint16_t sr = __m68k_state ? BE16(__m68k_state->SR) : 0u;
            uint32_t saved_pc = __m68k_state ? BE32(__m68k_state->PC) : 0u;
            kprintf("[EMU68-LIVE] frame=%llu pc=%08x saved_pc=%08x sr=%04x "
                    "stopped=%u int32=%08x ipl=%u insn=%llu\n",
                    (unsigned long long)frame, (unsigned)pc,
                    (unsigned)saved_pc, (unsigned)sr,
                    (unsigned)(__m68k_state ? __m68k_state->STOPPED : 0u),
                    (unsigned)(__m68k_state ? __m68k_state->INT32 : 0u),
                    (unsigned)(__m68k_state ? __m68k_state->INT.IPL : 0u),
                    (unsigned long long)insn_count);
            checkpoint_index++;
        }
    }

#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
    {
        static uint32_t s_last_vec6c = 0xdeadbeefu;
        static uint32_t s_pc_ring[8];
        static uint32_t s_pc_ring_idx;
        BellatrixMachine *mwatch = bellatrix_machine_get();
        if (mwatch && mwatch->memory.chip_ram) {
            uint32_t v = bellatrix_chip_read32(&mwatch->memory, 0x6cu);
            if (v != s_last_vec6c) {
                unsigned r;
                kprintf("[VEC6C-CHANGE] %08x->%08x pc=%08x insn=%llu cck=%llu\n",
                        (unsigned)s_last_vec6c, (unsigned)v, (unsigned)pc,
                        (unsigned long long)insn_count,
                        (unsigned long long)mwatch->tick_count);
                kprintf("[VEC6C-RING]");
                for (r = 0; r < 8u; r++)
                    kprintf(" %08x",
                            (unsigned)s_pc_ring[(s_pc_ring_idx + r) & 7u]);
                kprintf("\n");
                s_last_vec6c = v;
            }
        }
        s_pc_ring[s_pc_ring_idx & 7u] = pc;
        s_pc_ring_idx++;
    }
    {
        static uint32_t s_sample_count;
        if ((++s_sample_count & 0x0fffu) == 0u) {
            RigelContext *rctx = bellatrix_machine_rigel_ctx();
            kprintf("[EMU68-SAMPLE] pc=%08x insn=%llu intena=%04x intreq=%04x "
                    "ipl=%u int32=%08x cck=%llu\n",
                    (unsigned)pc,
                    (unsigned long long)insn_count,
                    (unsigned)(rctx ? rigel_custom_read16(rctx, RIGEL_REG_INTENAR) : 0u),
                    (unsigned)(rctx ? rigel_custom_read16(rctx, RIGEL_REG_INTREQR) : 0u),
                    (unsigned)(rctx ? rigel_get_ipl(rctx) : 0u),
                    (unsigned)(__m68k_state ? __m68k_state->INT32 : 0u),
                    (unsigned long long)(bellatrix_machine_get() ? bellatrix_machine_get()->tick_count : 0u));
        }
    }
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

void bellatrix_emu68_publish_idle_cycles(uint32_t cycles)
{
    if (!cycles)
        return;

    if (PAL_Core_IsMulticoreEnabled())
        bellatrix_bridge_publish_cpu_cycles(cycles);
    else
        bellatrix_machine_advance_cpu_cycles(cycles);

    bellatrix_runtime_poll_from_emu68();
}

/* ---------------------------------------------------------------------------
 * Strong overrides: per-core chipset advance steps.
 *
 * bellatrix_runtime_host_step  — Core 1, full chipset.
 * bellatrix_runtime_io_step    — Core 0, physical IO.
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
    if (bellatrix_emu68_backend_set_overlay(overlay_enabled))
        return;

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

#if defined(BELLATRIX_TRACE_VECPAGE) && BELLATRIX_TRACE_VECPAGE
    /* Diagnostic (opt-in): fault-drive the vector page so every access to
     * 0x000-0xFFF reaches bellatrix_bus_access and low-memory corruption
     * writers are caught with their exact PC ([VEC-W]). ~15x boot slowdown
     * under QEMU/TCG — enable only for targeted hunts. */
    mmu_map(0x000000, 0x000000, 0x1000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
#endif
}

#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
/* Deferred variant of the vector-page trap: armed at a precise moment (e.g.
 * first level-3 delivery) so only the interesting window pays the fault
 * overhead. Called from the ExecutionLoop trace block. */
void bellatrix_trace_arm_vecpage(void)
{
    static int armed;
    if (armed)
        return;
    armed = 1;
    mmu_map(0x000000, 0x000000, 0x1000,
            MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
    kprintf("[VECPAGE-ARMED]\n");
}
#endif

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
    bellatrix_emu68_boards_reset();
    cpu_backend = cpu_backend_selected();
    cpu_backend_init_selected();

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

    /* Phase marker, flushed synchronously on Core 0 (Core 3 — the normal drainer
     * — is not launched yet, so there is no cross-core drain race, and USB init
     * has just finished so the bus is idle). If this is the LAST line seen on
     * real hardware, the stall is inside core_io_init()/usb_host_init(), not
     * later; the deferred console would otherwise hide exactly where it stops. */
    kprintf("[PHASE] host services up — entering launcher phase\n");
    console_log_drain();

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

    /* Chip RAM: map exactly the real chip RAM size. 0x100000-0x1FFFFF must
     * NOT be presented to the CPU as anonymous low RAM: the chipset/DMA side
     * only treats BELLATRIX_CHIP_RAM_SIZE as chip RAM, so a wider CPU window
     * makes Kickstart size chip as 2MB and place the supervisor stack and
     * exec structures in memory the chipset cannot see (runaway boot,
     * ISSUE-0037 — same divergence removed from the Musashi backend).
     * Accesses to 0x100000-0x1FFFFF fall through to the fault handler and
     * read as open bus, matching the harness memory map. */
    mmu_map(0x000000, 0x000000, BELLATRIX_CHIP_RAM_SIZE,
            MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);

    /* 0x100000-0x1FFFFF: chip RAM MIRROR, as on real hardware — OCS/ECS
     * Agnus ignores A20 for chip cycles, so the whole 2MB window decodes
     * into the fitted chip RAM. This matters concretely:
     *  - AROS ROMs take the reset SSP from the ROM header (0x11144EF9 →
     *    0x114EF9 on a 24-bit bus): with a mirror the early supervisor
     *    stack lands in real chip RAM (physically 0x014EF9), coherent with
     *    DMA. With open bus here instead, the first push kills the boot.
     *  - Kickstart's RAM sizing detects the wrap and correctly reports 1MB
     *    (anonymous non-mirrored RAM here made it size chip as 2MB and put
     *    exec structures where the chipset cannot see them, ISSUE-0037).
     * Emu68's generic 1:1 map would otherwise back this range with raw
     * DRAM, so the alias must be mapped explicitly. */
    if (BELLATRIX_CHIP_RAM_SIZE < 0x00200000u) {
        uintptr_t mirror_virt = BELLATRIX_CHIP_RAM_SIZE;
        while (mirror_virt < 0x00200000u) {
            mmu_map(0x000000, mirror_virt, BELLATRIX_CHIP_RAM_SIZE,
                    MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED, 0);
            mirror_virt += BELLATRIX_CHIP_RAM_SIZE;
        }
    }

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
    /* Core 0 owns physical IO in launcher and runtime. Core 3 remains parked
     * for future RTG/AHI work. The launcher cooperatively calls the same Host
     * Reactor used later by the supervisor.
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

    /* Preserve launcher costs separately: enumeration/MSC may legitimately
     * block before CPU/chipset launch and must not contaminate runtime maxima. */
    {
        CoreIOReactorStats launcher_io;
        uint64_t freq = PAL_Time_GetFrequency();
        core_io_reactor_get_stats(&g_runtime.io, &launcher_io);
        kprintf("[CORE0-IO-LAUNCHER] calls=%llu budget_miss=%u max=%lluus "
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

    core_chipset_init(&g_runtime.chipset,
                      bellatrix_machine_rigel_ctx(),
                      g_runtime.machine);

#ifdef BELLATRIX_ENABLE_MULTICORE
    /* Runtime phase begins: the launcher is done and the chipset context is
     * initialised, so bring up Core 2 (chipset) now. Deferring it to here (from
     * before the launcher) keeps the launcher phase free of a second core
     * touching shared state (ISSUE-0042/0044). */
    PAL_Core_LaunchChipset(NULL);   /* Core 2 — chipset */
#if defined(BELLATRIX_EMU68_CORE0_REBASELINE) && \
    BELLATRIX_EMU68_CORE0_REBASELINE
    /* Core 0 enters the native Emu68 loop and cannot host the physical
     * reactor. Reuse the already-defined bounded IO worker on Core 3. */
    PAL_Core_LaunchIO();            /* Core 3 — physical IO reactor */
#endif
#endif

    kprintf("[BELA] build: " __DATE__ " " __TIME__ "\n");
    if (PAL_Core_IsMulticoreEnabled()) {
#if defined(BELLATRIX_EMU68_CORE0_REBASELINE) && \
    BELLATRIX_EMU68_CORE0_REBASELINE
        kprintf("[BELA] Initialized (Core0=Emu68 Core1=Aux Core2=Chipset Core3=IO)\n");
#else
        kprintf("[BELA] Initialized (Core0=Supervisor/IO Core1=CPU Core2=Chipset Core3=Reserved)\n");
#endif
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

#ifdef BELLATRIX_LAUNCHER
void bellatrix_launcher_pump_usb(void)
{
    /* Compatibility service point for launcher.c. It no longer drives USB
     * directly: launcher and runtime share the sole Core 0 Host Reactor. */
    static uint64_t last_tick;
    uint64_t now = PAL_Time_ReadCounter();
    uint64_t freq = PAL_Time_GetFrequency();
    uint64_t interval = freq / 1000u ? freq / 1000u : 1u;
    if (!last_tick || now - last_tick >= interval) {
        last_tick = now;
        bellatrix_runtime_io_step(now, freq);
    }
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

        if (dir == BUS_WRITE && addr < 0x100u)
        {
            kprintf("[VEC-W] addr=%06x val=%08x size=%d pc=%08x\n",
                    (unsigned)addr, (unsigned)value, size,
                    (unsigned)real_pc);
        }

        /* SetIntVector guts (KS13 fc11ca-fc120e): by the Disable() write at
         * fc11d2, A0 = ExecBase+0x54+12*vecnum. Dump the registers as of the
         * last MainLoop context spill to identify bogus args (ISSUE-0038). */
        if (dir == BUS_WRITE && addr == 0xDFF09Au &&
            real_pc >= 0xfc11c0u && real_pc <= 0xfc1210u && __m68k_state)
        {
            kprintf("[SETINTVEC] pc=%08x d0=%08x d1=%08x a0=%08x a1=%08x "
                    "a6=%08x usp=%08x\n",
                    (unsigned)real_pc,
                    (unsigned)BE32(__m68k_state->D[0].u32),
                    (unsigned)BE32(__m68k_state->D[1].u32),
                    (unsigned)BE32(__m68k_state->A[0].u32),
                    (unsigned)BE32(__m68k_state->A[1].u32),
                    (unsigned)BE32(__m68k_state->A[6].u32),
                    (unsigned)BE32(__m68k_state->USP.u32));
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
