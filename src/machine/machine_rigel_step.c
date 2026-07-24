// src/machine/machine_rigel_step.c
//
// Quantum/step helpers (machine_next_quantum, machine_quantum_step,
// machine_step_components, machine_flush_for_bus), video
// (machine_present_frame_from_rigel), serial step, input sync,
// and public step API (bellatrix_machine_advance,
// bellatrix_machine_recommended_cpu_quantum, bellatrix_machine_on_frame_ready,
// bellatrix_machine_audio_*, bellatrix_machine_serial_*).

#include "machine/machine_rigel_internal.h"

#include "audio/output.h"
#include "machine/memory/chip_ram.h"
#include "host/pal.h"
#include "host/osd.h"
#include "runtime/core_chipset.h"
#include "runtime/core_io.h"
#include "cpu/emu68/bellatrix_profile.h"
#include "support.h"
#if defined(BELLATRIX_EMU68_LIVENESS_TRACE) && BELLATRIX_EMU68_LIVENESS_TRACE
#include "M68k.h"
#endif

#include "rigel/rigel.h"
#include "core/rigel_context.h"   /* CIA_State watcher (ISSUE-0038 diagnosis) */
#include "rigel/rigel_audio.h"
#include "rigel/rigel_custom.h"
#include "rigel/rigel_input.h"
#include "rigel/rigel_irq.h"
#include "rigel/rigel_serial.h"
#include "host/raspi3/console_log.h"
#include "host/raspi3/hdmi_audio.h"
#ifdef BELLATRIX_LAUNCHER
#include "launcher/launcher.h"
#endif

#ifndef BELLATRIX_ENABLE_HDMI_AUDIO
#define BELLATRIX_ENABLE_HDMI_AUDIO 0
#endif

#ifdef BELLATRIX_HARNESS
#include "machine/expansions/rtg/rtg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

/* Weak fallback: tools/harness doesn't link host/raspi3/console_log.c (no
 * bare-metal mini-UART there); the raspi3 build's strong definition
 * overrides this when linked. */
__attribute__((weak)) void console_log_drain(void) {}

/* The POSIX harness does not link the bare-metal Core-3 runtime. Multicore is
 * always disabled there, but these references still exist in the compiled
 * function body. Strong core_io.c definitions override the fallbacks in the
 * Raspberry Pi image. */
__attribute__((weak)) bool core_io_serial_enqueue_tx(uint8_t byte)
{
    (void)byte;
    return false;
}

__attribute__((weak)) bool core_io_serial_dequeue_rx(uint8_t *byte_out)
{
    (void)byte_out;
    return false;
}

/* ---------------------------------------------------------------------------
 * Step accumulators — defined here; exported via internal header for bus.c
 * ------------------------------------------------------------------------- */

uint64_t      s_cpu_approx;  /* accumulated approximate Rigel CCKs      */
uint32_t      s_cpu_cck_rem; /* odd CPU-cycle remainder for /2 scaling  */
rigel_cycle_t s_quantum;     /* cycles to next Rigel event (the budget) */

typedef enum MachineStepReason {
    MACHINE_STEP_MAX = 0,
    MACHINE_STEP_DEADLINE,
    MACHINE_STEP_BUS_CHANGE,
    MACHINE_STEP_MMIO_FLUSH,
    MACHINE_STEP_REASON_COUNT
} MachineStepReason;

static MachineStepReason s_quantum_reason = MACHINE_STEP_MAX;

/* ---------------------------------------------------------------------------
 * Video helpers
 * ------------------------------------------------------------------------- */

static uint16_t machine_rgb8888_to_le565(uint32_t rgba)
{
    /* Rigel palette: 0x00RRGGBB (alpha=0 in high byte, R in bits 23-16) */
    uint8_t r8 = (uint8_t)((rgba >> 16) & 0xFFu);
    uint8_t g8 = (uint8_t)((rgba >>  8) & 0xFFu);
    uint8_t b8 = (uint8_t)( rgba        & 0xFFu);
    uint16_t rgb565 = (uint16_t)(((r8 >> 3) << 11) |
                                 ((g8 >> 2) << 5) |
                                 (b8 >> 3));
    return LE16(rgb565);
}

#ifdef BELLATRIX_HARNESS
static uint16_t machine_rgba_bytes_to_le565(const uint8_t *rgba)
{
    uint16_t rgb565 = (uint16_t)(((rgba[0] >> 3) << 11) |
                                 ((rgba[1] >> 2) << 5) |
                                 (rgba[2] >> 3));
    return LE16(rgb565);
}
#endif

#ifdef BELLATRIX_HARNESS
static int machine_perf_trace_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *env = getenv("HARNESS_PERF_TRACE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }

    return enabled;
}

typedef struct HarnessPerfTrace {
    uint64_t last_print;
    uint64_t step_calls;
    uint64_t step_cck;
    uint64_t rigel_ticks;
    uint64_t post_ticks;
    uint64_t present_ticks;
    uint64_t present_calls;
    uint64_t present_skips;
    uint64_t reason_calls[MACHINE_STEP_REASON_COUNT];
    uint64_t size_buckets[6]; /* 1, 2-4, 5-8, 9-32, 33-128, >128 */
} HarnessPerfTrace;

static HarnessPerfTrace s_perf_trace;

static uint32_t machine_video_skip_interval(void)
{
    static int initialized = 0;
    static uint32_t interval = 0;

    if (!initialized) {
        const char *env = getenv("HARNESS_VIDEO_SKIP");
        char *end = NULL;
        unsigned long value = 0;

        if (env && env[0] != '\0') {
            value = strtoul(env, &end, 10);
            if (end != env && value <= 60ul) {
                interval = (uint32_t)value;
            }
        }
        initialized = 1;
    }

    return interval;
}
#endif

#ifndef BELLATRIX_HARNESS
static uint32_t machine_dirty_line_count(const rigel_frame_t *frame)
{
    uint32_t count = 0;
    unsigned i;

    if (!frame)
        return 0;

    for (i = 0; i < 5u; ++i) {
        uint64_t v = frame->delta.dirty_lines[i];
        while (v) {
            count += (uint32_t)(v & 1u);
            v >>= 1;
        }
    }

    return count;
}

typedef struct MachineFrameStats {
    uint32_t bg;
    uint32_t hash;
    uint32_t samples;
    uint32_t non_bg;
} MachineFrameStats;

static MachineFrameStats machine_frame_stats_sample(const rigel_frame_t *frame)
{
    MachineFrameStats stats = {0, 2166136261u, 0, 0};
    uint32_t step_x;
    uint32_t step_y;
    uint32_t y;
    const uint32_t *pixels;

    if (!frame || !frame->pixels || frame->width == 0u || frame->height == 0u)
        return stats;

    pixels = (const uint32_t *)frame->pixels;
    stats.bg = pixels[0];
    step_x = (frame->width  + 31u) / 32u;
    step_y = (frame->height + 31u) / 32u;
    if (step_x == 0u) step_x = 1u;
    if (step_y == 0u) step_y = 1u;

    for (y = 0u; y < frame->height; y += step_y) {
        const uint32_t *row =
            (const uint32_t *)((const uint8_t *)frame->pixels + y * frame->pitch);
        uint32_t x;
        for (x = 0u; x < frame->width; x += step_x) {
            uint32_t px = row[x];
            stats.hash ^= px;
            stats.hash *= 16777619u;
            stats.samples++;
            if (px != stats.bg)
                stats.non_bg++;
        }
    }

    return stats;
}

static void machine_trace_baremetal_video_frame(const rigel_frame_t *frame)
{
    static uint32_t trace_count;
    static uint16_t last_bplcon0;
    static uint16_t last_dmacon;
    static uint16_t last_diwstrt;
    static uint16_t last_diwstop;
    static uint16_t last_ddfstrt;
    static uint16_t last_ddfstop;
    static uint16_t last_color00;
    static uint8_t last_valid;
    uint32_t f;
    uint32_t sample0 = 0;
    uint32_t sample_mid = 0;
    uint16_t bplcon0;
    uint16_t dmacon;
    uint16_t diwstrt;
    uint16_t diwstop;
    uint16_t ddfstrt;
    uint16_t ddfstop;
    uint16_t color00;
    uint8_t state_changed;
    MachineFrameStats stats;

    if (!frame)
        return;

    f = (uint32_t)frame->frame_count;
    bplcon0 = rigel_custom_read16(g_rigel, RIGEL_REG_BPLCON0);
    dmacon = rigel_custom_read16(g_rigel, RIGEL_REG_DMACON);
    diwstrt = rigel_custom_read16(g_rigel, RIGEL_REG_DIWSTRT);
    diwstop = rigel_custom_read16(g_rigel, RIGEL_REG_DIWSTOP);
    ddfstrt = rigel_custom_read16(g_rigel, RIGEL_REG_DDFSTRT);
    ddfstop = rigel_custom_read16(g_rigel, RIGEL_REG_DDFSTOP);
    color00 = rigel_custom_read16(g_rigel, RIGEL_REG_COLOR00);

    state_changed = !last_valid ||
                    bplcon0 != last_bplcon0 ||
                    dmacon != last_dmacon ||
                    diwstrt != last_diwstrt ||
                    diwstop != last_diwstop ||
                    ddfstrt != last_ddfstrt ||
                    ddfstop != last_ddfstop ||
                    color00 != last_color00;

    if (trace_count >= 16u && !state_changed && (f & 0xffu) != 0u)
        return;

    last_bplcon0 = bplcon0;
    last_dmacon = dmacon;
    last_diwstrt = diwstrt;
    last_diwstop = diwstop;
    last_ddfstrt = ddfstrt;
    last_ddfstop = ddfstop;
    last_color00 = color00;
    last_valid = 1u;

    if (frame->pixels && frame->width && frame->height) {
        const uint32_t *row0 = (const uint32_t *)frame->pixels;
        const uint32_t *row_mid =
            (const uint32_t *)((const uint8_t *)frame->pixels +
                               (frame->height / 2u) * frame->pitch);
        sample0 = row0[0];
        sample_mid = row_mid[frame->width / 2u];
    }
    stats = machine_frame_stats_sample(frame);

    kprintf("[VIDEO-BM] frame=%u src=%ux%u pitch=%u fmt=%u flags=0x%02x "
            "dirty=%u full=%u fb=%ux%u pitch=%u zero_copy=%u "
            "bplcon0=%04x depth=%u dmacon=%04x bplen=%u diw=%04x/%04x ddf=%04x/%04x "
            "color00=%04x px=%08x/%08x sig=%08x nonbg=%u/%u\n",
            (unsigned)f,
            (unsigned)frame->width,
            (unsigned)frame->height,
            (unsigned)frame->pitch,
            (unsigned)frame->format,
            (unsigned)frame->flags,
            (unsigned)machine_dirty_line_count(frame),
            frame->delta.full_redraw ? 1u : 0u,
            (unsigned)fb_width,
            (unsigned)fb_height,
            (unsigned)pitch,
            g_rigel_zero_copy_video ? 1u : 0u,
            (unsigned)bplcon0,
            (unsigned)((bplcon0 >> 12) & 7u),
            (unsigned)dmacon,
            (dmacon & RIGEL_DMACON_BPLEN) ? 1u : 0u,
            (unsigned)diwstrt,
            (unsigned)diwstop,
            (unsigned)ddfstrt,
            (unsigned)ddfstop,
            (unsigned)color00,
            (unsigned)sample0,
            (unsigned)sample_mid,
            (unsigned)stats.hash,
            (unsigned)stats.non_bg,
            (unsigned)stats.samples);

    trace_count++;
}
#endif

void machine_present_frame_from_rigel(void)
{
    rigel_frame_t frame;
#ifdef BELLATRIX_HARNESS
    BellatrixRtgFrame rtg_frame;
    PAL_VideoRect rtg_dirty[RTG_DIRTY_MAX_RECTS];
    bool use_rtg = false;
#endif
    uint32_t x, y;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t dst_x0;
    uint32_t dst_y0;
    const uint32_t *src;

#ifdef BELLATRIX_HARNESS
    {
        static uint32_t s_present_count = 0;
        uint32_t skip = machine_video_skip_interval();

        if (skip > 0u && (s_present_count++ % (skip + 1u)) != 0u) {
            if (machine_perf_trace_enabled())
                s_perf_trace.present_skips++;
            return;
        }
    }
#endif

#ifdef BELLATRIX_HARNESS
    use_rtg = bellatrix_rtg_get_frame(&rtg_frame) != 0;
    if (use_rtg) {
        uint32_t i;
        for (i = 0u; i < rtg_frame.dirty_count; ++i) {
            rtg_dirty[i].x = rtg_frame.dirty[i].x;
            rtg_dirty[i].y = rtg_frame.dirty[i].y;
            rtg_dirty[i].w = rtg_frame.dirty[i].w;
            rtg_dirty[i].h = rtg_frame.dirty[i].h;
        }
        PAL_Video_SetRTGSprite(rtg_frame.sprite_pixels,
                               rtg_frame.sprite_x, rtg_frame.sprite_y,
                               rtg_frame.sprite_w, rtg_frame.sprite_h,
                               rtg_frame.sprite_visible,
                               rtg_frame.sprite_image_changed,
                               rtg_frame.sprite_changed);
        if (PAL_Video_PresentRGBARegions(
                rtg_frame.pixels, rtg_frame.width, rtg_frame.height,
                rtg_frame.pitch, rtg_dirty, rtg_frame.dirty_count,
                rtg_frame.full_update))
            return;
    }
#endif

    if (g_rigel_zero_copy_video
#ifdef BELLATRIX_HARNESS
        && !use_rtg
#endif
    ) {
        PAL_Video_Flip();
#ifdef BELLATRIX_HARNESS
        if (machine_perf_trace_enabled())
            s_perf_trace.present_calls++;
#endif
        return;
    }

    if (!framebuffer || !pitch)
        return;

#ifdef BELLATRIX_HARNESS
    if (use_rtg) {
        memset(&frame, 0, sizeof(frame));
        frame.pixels = rtg_frame.pixels;
        frame.width = rtg_frame.width;
        frame.height = rtg_frame.height;
        frame.pitch = rtg_frame.pitch;
        frame.format = RIGEL_PIXEL_RGBA8888;
    } else
#endif
    {
        if (!g_rigel)
            return;

        /* Fetch the Denise frame descriptor coherently. The pixel copy stays
         * outside the lock; a race can tear pixels but not memory bounds. */
        bool frame_ok;

        core_chipset_lock_acquire();
        frame_ok = rigel_get_frame(g_rigel, &frame);
        core_chipset_lock_release();
        if (!frame_ok)
            return;
    }

#ifndef BELLATRIX_HARNESS
    if (machine_rigel_video_trace_enabled())
        machine_trace_baremetal_video_frame(&frame);
#endif

    /* When DIWSTRT/DIWSTOP are both 0 (cleared at reset), display_window_update
     * produces width=1, height=1 pointing to the VBLANK region.  Skip the PAL
     * VBLANK (26 lines) and fall back to the standard 320×256 active area. */
    if (
#ifdef BELLATRIX_HARNESS
        !use_rtg &&
#endif
        (frame.width < 16u || frame.height < 16u)) {
        frame.pixels = (const rigel_u32 *)((const uint8_t *)frame.pixels
                                           + 26u * frame.pitch);
        frame.width  = 320u;
        frame.height = 256u;
    }

    /* Guard: VBL copper writes (e.g. DIWSTRT=0xffff before BPLCON0 depth=0) can
     * produce out-of-range visible_y_start, causing rigel_get_frame to return a
     * nonsensical height via uint wrap.  Skip the frame to avoid reading past the
     * frame buffer and corrupting the SDL surface. */
    if (!frame.pixels || frame.width > 1920u || frame.height > 1080u)
        return;

    stride = pitch / 2u;
    src = frame.pixels;

    if (!src || frame.width == 0u || frame.height == 0u)
        return;

#ifndef BELLATRIX_HARNESS
    if ((frame.width != fb_width || frame.height != fb_height) &&
        PAL_Video_Resize(frame.width, frame.height, 16u) == 0) {
        stride = pitch / 2u;
    }
#endif

    uint32_t scale_x = (frame.width <= fb_width) ? (fb_width / frame.width) : 1u;
    uint32_t scale_y = (frame.height <= fb_height) ? (fb_height / frame.height) : 1u;
    uint32_t dst_w;
    uint32_t dst_h;
    if (scale_x < 1u) scale_x = 1u;
    if (scale_y < 1u) scale_y = 1u;

    dst_w = frame.width * scale_x;
    dst_h = frame.height * scale_y;
    if (dst_w > fb_width)
        dst_w = fb_width;
    if (dst_h > fb_height)
        dst_h = fb_height;

    width = dst_w;
    height = dst_h;

    if (width == 0u || height == 0u)
        return;

    dst_x0 = (fb_width > width) ? (fb_width - width) / 2u : 0u;
    dst_y0 = (fb_height > height) ? (fb_height - height) / 2u : 0u;

    {
        static uint32_t last_fb_width;
        static uint32_t last_fb_height;
        static uint32_t last_dst_x0;
        static uint32_t last_dst_y0;
        static uint32_t last_width;
        static uint32_t last_height;
        static uint16_t last_bg;
        static uint8_t initialized;
        uint16_t bg =
#ifdef BELLATRIX_HARNESS
            use_rtg ? machine_rgba_bytes_to_le565((const uint8_t *)src) :
#endif
            machine_rgb8888_to_le565(src[0]);

        if (!initialized ||
            fb_width != last_fb_width ||
            fb_height != last_fb_height ||
            dst_x0 != last_dst_x0 ||
            dst_y0 != last_dst_y0 ||
            width != last_width ||
            height != last_height ||
            bg != last_bg) {
            for (y = 0; y < fb_height; ++y) {
                uint16_t *drow = framebuffer + y * stride;
                for (x = 0; x < fb_width; ++x)
                    drow[x] = bg;
            }

            last_fb_width = fb_width;
            last_fb_height = fb_height;
            last_dst_x0 = dst_x0;
            last_dst_y0 = dst_y0;
            last_width = width;
            last_height = height;
            last_bg = bg;
            initialized = 1u;
        }
    }

    for (y = 0; y < height; ++y) {
        uint32_t src_y = (frame.height > height)
            ? (uint32_t)(((uint64_t)y * frame.height) / height)
            : (y / scale_y);
        const uint32_t *row = (const uint32_t *)((const uint8_t *)src + src_y * frame.pitch);
        uint16_t *drow = framebuffer + (dst_y0 + y) * stride;
        for (x = 0; x < width; ++x) {
            uint32_t src_x = (frame.width > width)
                ? (uint32_t)(((uint64_t)x * frame.width) / width)
                : (x / scale_x);
#ifdef BELLATRIX_HARNESS
            if (use_rtg) {
                const uint8_t *pixel = (const uint8_t *)row + src_x * 4u;
                drow[dst_x0 + x] = machine_rgba_bytes_to_le565(pixel);
            } else
#endif
            {
                drow[dst_x0 + x] = machine_rgb8888_to_le565(row[src_x]);
            }
        }
    }

    PAL_Video_Flip();

#ifdef BELLATRIX_HARNESS
    if (machine_perf_trace_enabled())
        s_perf_trace.present_calls++;
#endif
}

/* ---------------------------------------------------------------------------
 * Serial helpers
 * ------------------------------------------------------------------------- */

void machine_drain_serial_fallback_rigel(void)
{
    static char buf[256];
    static int pos = 0;
    uint8_t byte = 0;

    if (g_machine.uart_host.enabled || !g_rigel)
        return;

    while (rigel_serial_tx_available(g_rigel) &&
           rigel_serial_pop_tx_byte(g_rigel, &byte))
    {
        if (byte == '\0')
            continue;
        if (byte < 32 && byte != '\n' && byte != '\r' && byte != '\t')
            continue;
        if (byte == '\n' || byte == '\r' || pos >= (int)(sizeof(buf) - 1)) {
            buf[pos] = '\0';
            if (pos > 0)
                kprintf("[SERIAL] %s\n", buf);
            pos = 0;
            continue;
        }
        buf[pos++] = (char)byte;
    }
}

void machine_step_host_serial_rigel(void)
{
    uint8_t byte = 0;

    if (g_rigel && g_machine.uart_host.enabled) {
#ifndef BELLATRIX_HARNESS
        /* The harness has no IO core: core_io.c is not part of its build,
         * and PAL_Core_IsMulticoreEnabled() is always false there. */
        if (PAL_Core_IsMulticoreEnabled()) {
            /* Chipset owns Rigel and moves bytes only through SPSC queues.
             * Host reactor owns the physical UART and never accesses g_rigel. */
            while (rigel_serial_tx_available(g_rigel) &&
                   rigel_serial_pop_tx_byte(g_rigel, &byte))
                (void)core_io_serial_enqueue_tx(byte);

            while (core_io_serial_dequeue_rx(&byte))
                rigel_serial_receive_byte(g_rigel, byte);
        } else
#endif
        {
            while (rigel_serial_tx_available(g_rigel) &&
                   rigel_serial_pop_tx_byte(g_rigel, &byte))
            {
                if (!uart_host_send_byte(&g_machine.uart_host, byte))
                    break;
            }

            while (uart_host_receive_byte(&g_machine.uart_host, &byte))
                rigel_serial_receive_byte(g_rigel, byte);
        }
    }

    /* Single-core fallback owns physical IO locally. In multicore, only the
     * host reactor drains the console after servicing Paula's queue. */
    if (!PAL_Core_IsMulticoreEnabled())
        console_log_drain();
}

/* ---------------------------------------------------------------------------
 * Input sync
 * ------------------------------------------------------------------------- */

static uint16_t controller_port_joydat(const BellatrixControllerPortState *port)
{
    uint8_t x;
    uint8_t y;

    if (!port)
        return 0x0000u;

    if (port->device == BELLATRIX_CONTROLLER_PORT_JOYSTICK) {
        uint8_t y1 = port->joy_left ? 1u : 0u;
        uint8_t y0 = (uint8_t)(y1 ^ (port->joy_up ? 1u : 0u));
        uint8_t x1 = port->joy_right ? 1u : 0u;
        uint8_t x0 = (uint8_t)(x1 ^ (port->joy_down ? 1u : 0u));

        return (uint16_t)(((uint16_t)((uint8_t)((y1 << 1) | y0)) << 8) |
                          (uint16_t)((uint8_t)((x1 << 1) | x0)));
    }

    x = port->mouse_x;
    y = port->mouse_y;
    return (uint16_t)(((uint16_t)y << 8) | (uint16_t)x);
}

void machine_sync_controller_ports_rigel(BellatrixMachine *m)
{
    unsigned port;

    if (!m || !g_rigel)
        return;

    /* Callers run on the IO/presenter core (USB HID events, mouse frame
     * tick) while the Rigel owner steps concurrently — same access contract
     * as CPU-side MMIO, so take the chipset lock. No-op in single-core. */
    core_chipset_lock_acquire();
    for (port = 0u; port < 2u; ++port) {
        const BellatrixControllerPortState *state = &m->controller_ports.port[port];
        rigel_input_set_joydat(g_rigel, port, controller_port_joydat(state));
        rigel_input_set_fire(g_rigel, port, state->fire ? true : false);
        rigel_input_set_pot_button_x(g_rigel, port, state->button2 ? true : false);
        rigel_input_set_pot_button_y(g_rigel, port, state->button3 ? true : false);
    }
    core_chipset_lock_release();
}

/* ---------------------------------------------------------------------------
 * Quantum-based Rigel advancement.
 *
 * Rigel owns the CCK clock.  The JIT is an inexact tenant: bela_delta*8 tells
 * us approximately how many M68K CPU cycles have been consumed.  Bellatrix
 * CPU cycles run at roughly 2x the chipset CCK rate, so the integration
 * converts CPU cycles to CCK before deciding WHEN to step Rigel.  Rigel is
 * always advanced by the exact number of CCK cycles to the next event
 * deadline, never by the raw JIT estimate.
 *
 * Result: chipset timing (VBL, copper, DMA, audio) is cycle-exact.  CPU
 * speed is approximate, as on any emulator that lacks per-instruction timing.
 *
 * Bus accesses flush any partial accumulated cycles as a partial step so
 * that chipset register reads reflect the latest state.
 * ------------------------------------------------------------------------- */

/* Compute cycles to the nearest upcoming Rigel event deadline. */
rigel_cycle_t machine_next_quantum(void)
{
    rigel_cycle_t now  = rigel_get_time(g_rigel);
    rigel_cycle_t q    = RIGEL_MAX_QUANTUM;
    rigel_cycle_t next;

    s_quantum_reason = MACHINE_STEP_MAX;

    next = rigel_get_next_observable_deadline(g_rigel);
    if (next > now && (next - now) < q) {
        q = next - now;
        s_quantum_reason = MACHINE_STEP_DEADLINE;
    }

    if (q == 0u)
        return RIGEL_MIN_QUANTUM;

    return q;
}

/* Execute one quantum step and update machine state. Returns event flags. */
static rigel_event_flags_t machine_quantum_step(BellatrixMachine *m,
                                                rigel_cycle_t cycles,
                                                MachineStepReason reason)
{
    rigel_step_result_t r;
#ifdef BELLATRIX_HARNESS
    int perf = machine_perf_trace_enabled();
    uint64_t t0 = 0;
    uint64_t t1 = 0;
    uint64_t t2 = 0;
#else
    (void)reason;   /* consumed only by the harness perf trace */
#endif

    if (!m || !g_rigel || cycles == 0u) return 0;

#ifdef BELLATRIX_HARNESS
    if (perf)
        t0 = PAL_Time_ReadCounter();
#endif
    r = rigel_step(g_rigel, cycles);
#ifdef BELLATRIX_HARNESS
    if (perf)
        t1 = PAL_Time_ReadCounter();
#endif
    m->tick_count = (uint64_t)r.time;

    machine_rigel_trace_step(&r);
    machine_step_host_serial_rigel();
    machine_drain_serial_fallback_rigel();
    /* Refill CIA-A SDR from the keyboard queue every chipset advance.
     * post_chipset_step only runs from the multicore Core-1 loop; in
     * single-core the queue would otherwise drain one byte per *new*
     * keypress, delivering keystrokes several events late. */
    machine_keyboard_drain_rigel();

#ifdef BELLATRIX_HARNESS
    if (perf)
        t2 = PAL_Time_ReadCounter();
#endif

    if (r.events & RIGEL_EVENT_FRAME_READY) {
#ifdef BELLATRIX_HARNESS
        uint64_t frame_t0 = 0;
        if (perf)
            frame_t0 = PAL_Time_ReadCounter();
#endif
        bellatrix_machine_on_frame_ready();
#ifdef BELLATRIX_HARNESS
        if (perf)
            s_perf_trace.present_ticks += PAL_Time_ReadCounter() - frame_t0;
#endif
    }
    if (r.events & RIGEL_EVENT_IRQ_CHANGED)
        machine_publish_ipl(m, rigel_get_ipl(g_rigel));
    if (r.events & RIGEL_EVENT_HBLANK)
        bellatrix_machine_on_audio_sample_ready();
    bellatrix_audio_output_tick((uint32_t)cycles);

#if !defined(BELLATRIX_HARNESS) && BELLATRIX_ENABLE_HDMI_AUDIO
    /* Refill the IRQ-free HDMI-audio DMA ring from the output queue. The DMA,
     * paced by the HDMI DREQ, feeds the MAI FIFO continuously; here we only top
     * up whichever buffer half it just finished. Diagnostics live inside. */
    hdmi_audio_dma_poll();
#endif

#ifdef BELLATRIX_HARNESS
    if (perf) {
        uint64_t now = PAL_Time_ReadCounter();
        uint64_t freq = PAL_Time_GetFrequency();

        s_perf_trace.step_calls++;
        s_perf_trace.step_cck += cycles;
        if ((unsigned)reason < MACHINE_STEP_REASON_COUNT)
            s_perf_trace.reason_calls[reason]++;
        if (cycles <= 1u)
            s_perf_trace.size_buckets[0]++;
        else if (cycles <= 4u)
            s_perf_trace.size_buckets[1]++;
        else if (cycles <= 8u)
            s_perf_trace.size_buckets[2]++;
        else if (cycles <= 32u)
            s_perf_trace.size_buckets[3]++;
        else if (cycles <= 128u)
            s_perf_trace.size_buckets[4]++;
        else
            s_perf_trace.size_buckets[5]++;
        s_perf_trace.rigel_ticks += t1 - t0;
        s_perf_trace.post_ticks += t2 - t1;

        if (s_perf_trace.last_print == 0u) {
            s_perf_trace.last_print = now;
        } else if (now - s_perf_trace.last_print >= freq) {
            double scale = 1000.0 / (double)freq;
            /* Host load average stamps each measurement with the machine's
             * contention state; a loaded host invalidates absolute numbers
             * (see ISSUE-0048: orphaned QEMUs masqueraded as a regression). */
            double host_load = -1.0;
            (void)getloadavg(&host_load, 1);
            fprintf(stderr,
                    "[HARNESS-PERF] steps=%llu cck=%llu"
                    " rigel_ms=%.2f post_ms=%.2f present_ms=%.2f"
                    " present=%llu skip=%llu"
                    " reason=max:%llu deadline:%llu bus:%llu mmio:%llu"
                    " size=1:%llu 2-4:%llu 5-8:%llu 9-32:%llu"
                    " 33-128:%llu >128:%llu load=%.2f\n",
                    (unsigned long long)s_perf_trace.step_calls,
                    (unsigned long long)s_perf_trace.step_cck,
                    (double)s_perf_trace.rigel_ticks * scale,
                    (double)s_perf_trace.post_ticks * scale,
                    (double)s_perf_trace.present_ticks * scale,
                    (unsigned long long)s_perf_trace.present_calls,
                    (unsigned long long)s_perf_trace.present_skips,
                    (unsigned long long)s_perf_trace.reason_calls[MACHINE_STEP_MAX],
                    (unsigned long long)s_perf_trace.reason_calls[MACHINE_STEP_DEADLINE],
                    (unsigned long long)s_perf_trace.reason_calls[MACHINE_STEP_BUS_CHANGE],
                    (unsigned long long)s_perf_trace.reason_calls[MACHINE_STEP_MMIO_FLUSH],
                    (unsigned long long)s_perf_trace.size_buckets[0],
                    (unsigned long long)s_perf_trace.size_buckets[1],
                    (unsigned long long)s_perf_trace.size_buckets[2],
                    (unsigned long long)s_perf_trace.size_buckets[3],
                    (unsigned long long)s_perf_trace.size_buckets[4],
                    (unsigned long long)s_perf_trace.size_buckets[5],
                    host_load);
            s_perf_trace.last_print = now;
            s_perf_trace.step_calls = 0;
            s_perf_trace.step_cck = 0;
            s_perf_trace.rigel_ticks = 0;
            s_perf_trace.post_ticks = 0;
            s_perf_trace.present_ticks = 0;
            s_perf_trace.present_calls = 0;
            s_perf_trace.present_skips = 0;
            memset(s_perf_trace.reason_calls, 0,
                   sizeof(s_perf_trace.reason_calls));
            memset(s_perf_trace.size_buckets, 0,
                   sizeof(s_perf_trace.size_buckets));
        }
    }
#endif

    return r.events;
}

/*
 * Called with the approximate number of M68K cycles the JIT just consumed.
 * When that estimate reaches the current quantum, Rigel advances by the
 * EXACT quantum (not the estimate) and the next quantum is fetched.
 * Quantum is only recomputed when it is consumed or events fire.
 */
void machine_step_components(BellatrixMachine *m, uint32_t approx)
{
    if (!m || !g_rigel) return;

    if (s_quantum == 0u)
        s_quantum = machine_next_quantum();

    {
        uint64_t scaled = (uint64_t)s_cpu_cck_rem + approx;
        s_cpu_approx += scaled / 2u;
        s_cpu_cck_rem = (uint32_t)(scaled & 1u);
    }

    while (s_cpu_approx >= s_quantum) {
        s_cpu_approx -= s_quantum;
        rigel_event_flags_t ev = machine_quantum_step(m, s_quantum,
                                                      s_quantum_reason);
        /* Recompute deadline only when quantum consumed or events fired. */
        if (ev || s_cpu_approx >= s_quantum)
            s_quantum = machine_next_quantum();
    }
}

/*
 * Flush any accumulated partial cycles before a chipset bus access so that
 * register reads (VPOS, INTREQ, etc.) reflect the current beam position.
 * Advances Rigel by whatever is in s_cpu_approx as a partial step.
 */
void machine_flush_for_bus(BellatrixMachine *m)
{
    if (!m || !g_rigel || s_cpu_approx == 0u) return;

    rigel_cycle_t partial = (rigel_cycle_t)s_cpu_approx;
    if (s_quantum > 0u && partial > s_quantum)
        partial = s_quantum;

    rigel_event_flags_t ev = machine_quantum_step(m, partial,
                                                  MACHINE_STEP_MMIO_FLUSH);
    s_cpu_approx = (s_cpu_approx >= partial) ? s_cpu_approx - partial : 0u;

    /* Recompute deadline only if we hit the quantum boundary or events fired. */
    if (ev || partial >= s_quantum)
        s_quantum = machine_next_quantum();
}

/* ---------------------------------------------------------------------------
 * Public step API
 * ------------------------------------------------------------------------- */

void bellatrix_machine_advance(uint32_t ticks)
{
    machine_step_components(&g_machine, ticks);
}

uint32_t bellatrix_machine_cpu_chip_access(unsigned int word_transfers,
                                           unsigned int transfer_cck,
                                           int wait_for_slot)
{
    uint64_t waited = 0u;
    unsigned int transfer;

    if (!g_rigel || PAL_Core_IsMulticoreEnabled() ||
        word_transfers == 0u || transfer_cck == 0u)
        return 0u;

    machine_flush_for_bus(&g_machine);
    for (transfer = 0u; transfer < word_transfers; ++transfer) {
        while (wait_for_slot && rigel_cpu_would_stall(g_rigel)) {
            rigel_bus_state_t bus = rigel_get_bus_state(g_rigel);
            rigel_cycle_t now = rigel_get_time(g_rigel);
            rigel_cycle_t resume = bus.next_change;
            rigel_cycle_t delta = resume > now ? resume - now : 1u;

            (void)machine_quantum_step(&g_machine, delta,
                                       MACHINE_STEP_BUS_CHANGE);
            waited += delta;
        }

        /* The grant is the first CCK of the 68000's four-clock bus cycle.
         * The remaining tail still advances all chipset domains. */
        (void)machine_quantum_step(&g_machine, transfer_cck,
                                   MACHINE_STEP_BUS_CHANGE);
    }
    s_quantum = machine_next_quantum();

    return waited > UINT32_MAX ? UINT32_MAX : (uint32_t)waited;
}

void bellatrix_machine_on_frame_skipped(void)
{
    g_machine.frame_counter++;
    machine_mouse_frame_tick();
}

#if defined(BELLATRIX_PLANE_DIAG_BUILD) && BELLATRIX_PLANE_DIAG_BUILD
static uint32_t machine_plane_hash(uint32_t addr, uint32_t bytes)
{
    uint32_t hash = 2166136261u;
    uint32_t end;

    addr &= 0x00fffffeu;
    if (!chip_ram_is_configured(&g_machine.memory) ||
        addr >= g_machine.memory.chip_ram_size)
        return 0u;
    end = addr + bytes;
    if (end < addr || end > g_machine.memory.chip_ram_size)
        end = g_machine.memory.chip_ram_size;

    while (addr + 1u < end) {
        uint16_t word = chip_ram_read16(&g_machine.memory, addr);
        hash ^= (uint8_t)(word >> 8);
        hash *= 16777619u;
        hash ^= (uint8_t)word;
        hash *= 16777619u;
        addr += 2u;
    }
    return hash;
}

static uint32_t machine_direct_alias_hash(uint32_t addr, uint32_t bytes)
{
    volatile const uint8_t *p = (volatile const uint8_t *)(uintptr_t)addr;
    uint32_t hash = 2166136261u;
    uint32_t i;

    if (addr >= BELLATRIX_CHIP_CPU_APERTURE_SIZE ||
        bytes > BELLATRIX_CHIP_CPU_APERTURE_SIZE - addr)
        return 0u;
    for (i = 0u; i < bytes; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

static void machine_trace_plane_payload(void)
{
    uint32_t ptr[6];
    uint32_t hash[6];
    uint16_t bplcon0;
    unsigned i;

    if (!g_rigel)
        return;
    bplcon0 = rigel_custom_read16(g_rigel, RIGEL_REG_BPLCON0);
    for (i = 0u; i < 6u; i++) {
        uint32_t reg = RIGEL_REG_BPL1PTH + i * 4u;
        ptr[i] = ((uint32_t)rigel_custom_read16(g_rigel, reg) << 16) |
                 rigel_custom_read16(g_rigel, reg + 2u);
        hash[i] = machine_plane_hash(ptr[i], 0x2000u);
    }
    kprintf("[PLANE-DIAG] frame=%u bplcon0=%04x mod=%04x/%04x "
            "p1=%06x:%08x p2=%06x:%08x p3=%06x:%08x "
            "p4=%06x:%08x p5=%06x:%08x p6=%06x:%08x\n",
            (unsigned)g_machine.frame_counter,
            (unsigned)bplcon0,
            (unsigned)rigel_custom_read16(g_rigel, 0x108u),
            (unsigned)rigel_custom_read16(g_rigel, 0x10au),
            (unsigned)(ptr[0] & 0x00ffffffu), (unsigned)hash[0],
            (unsigned)(ptr[1] & 0x00ffffffu), (unsigned)hash[1],
            (unsigned)(ptr[2] & 0x00ffffffu), (unsigned)hash[2],
            (unsigned)(ptr[3] & 0x00ffffffu), (unsigned)hash[3],
            (unsigned)(ptr[4] & 0x00ffffffu), (unsigned)hash[4],
            (unsigned)(ptr[5] & 0x00ffffffu), (unsigned)hash[5]);

    if ((g_machine.frame_counter == 200u ||
         g_machine.frame_counter == 300u) && ptr[1] != 0u) {
        kprintf("[PLANE2-ALIAS] backing=%08x direct=%08x\n",
                (unsigned)machine_plane_hash(ptr[1], 0x2000u),
                (unsigned)machine_direct_alias_hash(ptr[1], 0x2000u));
    }
    if (g_machine.frame_counter == 300u && ptr[1] != 0u) {
        for (i = 0u; i < 32u; i++) {
            if ((i & 7u) == 0u)
                kprintf("[PLANE2-BLOCKS] base=%06x block=%02u",
                        (unsigned)(ptr[1] & 0x00ffffffu), i);
            kprintf(" %08x", (unsigned)machine_plane_hash(
                        ptr[1] + i * 0x100u, 0x100u));
            if ((i & 7u) == 7u)
                kprintf("\n");
        }
    }
}
#endif

void bellatrix_machine_on_frame_ready(void)
{
    g_machine.frame_counter++;
#if BELLATRIX_PROFILE_ENABLED
    /* Frame-pacing sample: fires once per presented frame in both single-core
     * and multicore, since this is the common frame-ready point. */
    bprof_frame_pacing();
#endif
    machine_mouse_frame_tick();
    osd_set_machine_frame(g_machine.frame_counter);

    /* Live speed readout, sampled over ~0.5 s so the figures are steady:
     *   realtime% = emulated CCK advanced per wall second vs the master clock
     *               (the "am I keeping up" metric; 100% = full-speed Amiga).
     *   fps       = presented frames per wall second (what the eye sees),
     *               measured directly so it needs no PAL/NTSC assumption.
     * realtime% feeds the on-screen OSD (which nothing else fed, so its RT
     * field previously read 000%); both go to the serial [PERF] line so the
     * rate is visible headless too. */
    {
        static uint64_t rt_last_wall  = 0;
        static uint64_t rt_last_cck   = 0;
        static uint64_t rt_last_frame = 0;
        uint64_t now  = PAL_Time_ReadCounter();
        uint64_t freq = PAL_Time_GetFrequency();
        if (rt_last_wall == 0u || freq == 0u) {
            rt_last_wall  = now;
            rt_last_cck   = g_machine.tick_count;
            rt_last_frame = g_machine.frame_counter;
        } else if ((now - rt_last_wall) >= freq / 2u) {
            uint64_t dwall   = now - rt_last_wall;
            uint64_t dcck    = g_machine.tick_count - rt_last_cck;
            uint64_t dframes = g_machine.frame_counter - rt_last_frame;
            /* Rigel time/beam positions are in CCK (227 per PAL line), while
             * config.clock_hz is the 68k clock. One CCK is two 68k clocks. */
            uint32_t cck_hz  =
                rigel_get_clock_hz(bellatrix_machine_rigel_ctx()) / 2u;
            uint64_t fps     = (dframes * freq) / dwall;
            if (cck_hz != 0u) {
                uint64_t pct = (dcck * freq * 100ull) / (dwall * (uint64_t)cck_hz);
                osd_set_realtime_percent((uint32_t)pct);
                kprintf("[PERF] realtime=%llu%%  fps=%llu\n",
                        (unsigned long long)pct,
                        (unsigned long long)fps);
            }
            rt_last_wall  = now;
            rt_last_cck   = g_machine.tick_count;
            rt_last_frame = g_machine.frame_counter;
        }
    }
    /* Runtime launcher screens share the physical framebuffer with Rigel.
     * Keep CPU/chipset/frame accounting alive, but preserve the modal until
     * host reactor closes it; the next frame restores the guest image. */
#ifdef BELLATRIX_LAUNCHER
    if (launcher_runtime_modal_active())
        return;
#endif
    machine_present_frame_from_rigel();

#if defined(BELLATRIX_PLANE_DIAG_BUILD) && BELLATRIX_PLANE_DIAG_BUILD
    if (g_machine.frame_counter == 200u ||
        g_machine.frame_counter == 300u ||
        g_machine.frame_counter == 500u ||
        g_machine.frame_counter == 1000u)
        machine_trace_plane_payload();
#endif

#if defined(BELLATRIX_EMU68_LIVENESS_TRACE) && BELLATRIX_EMU68_LIVENESS_TRACE
    if (g_machine.frame_counter == 1u ||
        (g_machine.frame_counter % 25u) == 0u) {
        extern struct M68KState *__m68k_state;
        uint32_t pc = __m68k_state ? BE32(__m68k_state->PC) : 0u;
        uint8_t guest_ipl = __m68k_state ? __m68k_state->INT.IPL : 0u;
        uint8_t physical_arm = __m68k_state ? __m68k_state->INT.ARM : 0u;
        kprintf("[EMU68-LIVE] frame=%u pc=%08x guest_ipl=%u arm_irq=%u\n",
                (unsigned)g_machine.frame_counter, (unsigned)pc,
                (unsigned)guest_ipl, (unsigned)physical_arm);
        {
            const CIA_State *ca = &g_rigel->chipset.cia[0];
            const CIA_State *cb = &g_rigel->chipset.cia[1];
            kprintf("[CIA-DIAG] frame=%u "
                    "A=%02x/%02x cra=%02x crb=%02x ta=%04x/%04x tb=%04x/%04x irq=%u "
                    "B=%02x/%02x cra=%02x crb=%02x ta=%04x/%04x tb=%04x/%04x irq=%u\n",
                    (unsigned)g_machine.frame_counter,
                    (unsigned)ca->icr_data, (unsigned)ca->icr_mask,
                    (unsigned)ca->cra, (unsigned)ca->crb,
                    (unsigned)ca->ta_counter, (unsigned)ca->ta_latch,
                    (unsigned)ca->tb_counter, (unsigned)ca->tb_latch,
                    (unsigned)ca->irq_asserted,
                    (unsigned)cb->icr_data, (unsigned)cb->icr_mask,
                    (unsigned)cb->cra, (unsigned)cb->crb,
                    (unsigned)cb->ta_counter, (unsigned)cb->ta_latch,
                    (unsigned)cb->tb_counter, (unsigned)cb->tb_latch,
                    (unsigned)cb->irq_asserted);
        }
#if defined(BELLATRIX_TRACE_BUILD) && BELLATRIX_TRACE_BUILD
        {
            extern uint32_t g_machine_cia_w_counts[2][16];
            extern uint32_t g_machine_cia_r_counts[2][16];
            extern uint8_t g_machine_cia_last_icr_w[2];
            kprintf("[EMU68-CIACNT] frame=%u "
                    "a_w_icr=%u(%02x) a_w_cr=%u/%u a_w_t=%u a_r_icr=%u "
                    "b_w_icr=%u(%02x) b_w_cr=%u/%u b_w_t=%u b_r_icr=%u\n",
                    (unsigned)g_machine.frame_counter,
                    (unsigned)g_machine_cia_w_counts[0][13],
                    (unsigned)g_machine_cia_last_icr_w[0],
                    (unsigned)g_machine_cia_w_counts[0][14],
                    (unsigned)g_machine_cia_w_counts[0][15],
                    (unsigned)(g_machine_cia_w_counts[0][4] +
                               g_machine_cia_w_counts[0][5] +
                               g_machine_cia_w_counts[0][6] +
                               g_machine_cia_w_counts[0][7]),
                    (unsigned)g_machine_cia_r_counts[0][13],
                    (unsigned)g_machine_cia_w_counts[1][13],
                    (unsigned)g_machine_cia_last_icr_w[1],
                    (unsigned)g_machine_cia_w_counts[1][14],
                    (unsigned)g_machine_cia_w_counts[1][15],
                    (unsigned)(g_machine_cia_w_counts[1][4] +
                               g_machine_cia_w_counts[1][5] +
                               g_machine_cia_w_counts[1][6] +
                               g_machine_cia_w_counts[1][7]),
                    (unsigned)g_machine_cia_r_counts[1][13]);
        }
        {
            extern uint32_t g_bela_irq_deliver_count;
            extern uint32_t g_bela_irq_deliver_ring[8];
            unsigned r;
            kprintf("[EMU68-DELIV] frame=%u count=%u ring=",
                    (unsigned)g_machine.frame_counter,
                    (unsigned)g_bela_irq_deliver_count);
            for (r = 0u; r < 8u; r++)
                kprintf(" %08x", (unsigned)g_bela_irq_deliver_ring[r]);
            kprintf("\n");
        }
        {
            const FloppyDrive *df0 = &g_rigel->chipset.floppy[0];
            kprintf("[EMU68-DF0] frame=%u conn=%u cyl=%u motor=%u rdy=%u "
                    "chgd=%u idcnt=%u ins=%u\n",
                    (unsigned)g_machine.frame_counter,
                    (unsigned)df0->connected, (unsigned)df0->cylinder,
                    (unsigned)df0->motor, (unsigned)df0->ready,
                    (unsigned)df0->disk_changed, (unsigned)df0->id_count,
                    (unsigned)df0->disk_inserted);
        }
        {
            const CIA_State *ca = &g_rigel->chipset.cia[0];
            const CIA_State *cb = &g_rigel->chipset.cia[1];
            kprintf("[EMU68-CIAST] frame=%u "
                    "A: icr=%02x/%02x cra=%02x crb=%02x ta=%04x/%04x tb=%04x/%04x irq=%u | "
                    "B: icr=%02x/%02x cra=%02x crb=%02x irq=%u\n",
                    (unsigned)g_machine.frame_counter,
                    (unsigned)ca->icr_data, (unsigned)ca->icr_mask,
                    (unsigned)ca->cra, (unsigned)ca->crb,
                    (unsigned)ca->ta_counter, (unsigned)ca->ta_latch,
                    (unsigned)ca->tb_counter, (unsigned)ca->tb_latch,
                    (unsigned)ca->irq_asserted,
                    (unsigned)cb->icr_data, (unsigned)cb->icr_mask,
                    (unsigned)cb->cra, (unsigned)cb->crb,
                    (unsigned)cb->irq_asserted);
        }
        {
            extern uint32_t g_machine_ipl_rise_counts[8];
            kprintf("[EMU68-IPLCNT] frame=%u rise1=%u rise2=%u rise3=%u "
                    "rise4=%u rise5=%u rise6=%u rise7=%u\n",
                    (unsigned)g_machine.frame_counter,
                    (unsigned)g_machine_ipl_rise_counts[1],
                    (unsigned)g_machine_ipl_rise_counts[2],
                    (unsigned)g_machine_ipl_rise_counts[3],
                    (unsigned)g_machine_ipl_rise_counts[4],
                    (unsigned)g_machine_ipl_rise_counts[5],
                    (unsigned)g_machine_ipl_rise_counts[6],
                    (unsigned)g_machine_ipl_rise_counts[7]);
        }
#endif
    }
#endif

    if (machine_rigel_trace_verbose_enabled())
        kprintf("[RIGEL-AUDIO-QUEUE] count=%u dropped=%llu\n",
                (unsigned)audio_mixer_count(&g_machine.audio_queue),
                (unsigned long long)audio_mixer_dropped(&g_machine.audio_queue));
}

void bellatrix_machine_on_ipl_changed(uint8_t ipl)
{
    machine_publish_ipl(&g_machine, ipl);
}

/* Drains one mixed stereo sample per RIGEL_EVENT_HBLANK (~15 kHz at PAL),
 * the rate rigel_audio.h documents as the natural tick for this getter.
 * No host output driver consumes the queue yet — this exists so the
 * fetch/mix/queue path has a real, regularly-driven caller to validate
 * against, per AI_context/issue_paula_audio_timing_and_simd.md. */
void bellatrix_machine_on_audio_sample_ready(void)
{
    audio_mixer_push(&g_machine.audio_queue,
                     bellatrix_machine_audio_left(),
                     bellatrix_machine_audio_right());
}

void bellatrix_machine_on_chipset_advanced(uint32_t cck_cycles)
{
    bellatrix_audio_output_tick(cck_cycles);
}

void bellatrix_machine_host_audio_poll(void)
{
#if !defined(BELLATRIX_HARNESS) && BELLATRIX_ENABLE_HDMI_AUDIO
    hdmi_audio_dma_poll();
#endif
}

void bellatrix_machine_post_chipset_step(void)
{
    /* Runs on the Rigel owner core but OUTSIDE its stepping lock section,
     * concurrently with CPU-side MMIO that holds the lock — and all three
     * helpers mutate Rigel state (serial registers, CIA SDR). Same access
     * contract as everyone else: take the lock. No-op in single-core. */
    core_chipset_lock_acquire();
    machine_step_host_serial_rigel();
    machine_drain_serial_fallback_rigel();
    machine_keyboard_drain_rigel();
    core_chipset_lock_release();
}

void bellatrix_machine_sync_ipl(void)
{
    if (g_rigel) {
        uint8_t ipl = (uint8_t)rigel_get_ipl(g_rigel);
        if (PAL_Core_IsMulticoreEnabled())
            core_chipset_set_pending_ipl(ipl);
        machine_publish_ipl(&g_machine, ipl);
    }
}

uint32_t bellatrix_machine_recommended_cpu_quantum(uint32_t max_cycles)
{
    rigel_cycle_t now;
    rigel_cycle_t next;
    rigel_cycle_t cck_quantum = RIGEL_MAX_QUANTUM;
    uint64_t cpu_quantum;

    if (!g_rigel)
        return max_cycles != 0u ? max_cycles : 1u;

#ifdef BELLATRIX_HARNESS
    {
        static int fixed_quantum = -1;

        if (fixed_quantum < 0) {
            const char *env = getenv("HARNESS_CPU_DEADLINE_QUANTUM");
            fixed_quantum = (env && env[0] != '\0' && strcmp(env, "0") != 0) ? 0 : 1;
        }

        if (fixed_quantum)
            return max_cycles != 0u ? max_cycles : 1u;
    }
#endif

    now = rigel_get_time(g_rigel);

    next = rigel_get_next_observable_deadline(g_rigel);
    if (next > now && (next - now) < cck_quantum)
        cck_quantum = next - now;

    if (cck_quantum == 0u)
        return 1u;

    /*
     * The harness consumes this value as approximate M68K CPU cycles, while
     * Rigel deadlines are in CCK.  Account for the /2 conversion and any odd
     * CPU-cycle remainder already carried by the integrator.
     */
    cpu_quantum = ((uint64_t)cck_quantum * 2u);
    if (s_cpu_cck_rem != 0u && cpu_quantum > 1u)
        cpu_quantum -= 1u;

    if (max_cycles != 0u && cpu_quantum > max_cycles)
        cpu_quantum = max_cycles;

    return cpu_quantum != 0u ? (uint32_t)cpu_quantum : 1u;
}
