// src/machine/machine_rigel_step.c
//
// Quantum/step helpers (machine_next_quantum, machine_quantum_step,
// machine_step_components, machine_flush_for_bus), video
// (machine_present_frame_from_rigel), serial step, input sync,
// and public step API (bellatrix_machine_advance,
// bellatrix_machine_recommended_cpu_quantum, bellatrix_machine_on_frame_ready,
// bellatrix_machine_audio_*, bellatrix_machine_serial_*).

#include "machine/machine_rigel_internal.h"

#include "machine/memory/chip_ram.h"
#include "host/pal.h"
#include "support.h"

#include "rigel/rigel.h"
#include "rigel/rigel_audio.h"
#include "rigel/rigel_custom.h"
#include "rigel/rigel_input.h"
#include "rigel/rigel_irq.h"
#include "rigel/rigel_serial.h"
#include "host/raspi3/console_log.h"

/* Weak fallback: tools/harness doesn't link host/raspi3/console_log.c (no
 * bare-metal mini-UART there); the raspi3 build's strong definition
 * overrides this when linked. */
__attribute__((weak)) void console_log_drain(void) {}

/* ---------------------------------------------------------------------------
 * Step accumulators — defined here; exported via internal header for bus.c
 * ------------------------------------------------------------------------- */

uint64_t      s_cpu_approx;  /* accumulated approximate Rigel CCKs      */
uint32_t      s_cpu_cck_rem; /* odd CPU-cycle remainder for /2 scaling  */
rigel_cycle_t s_quantum;     /* cycles to next Rigel event (the budget) */

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

void machine_present_frame_from_rigel(void)
{
    rigel_frame_t frame;
    uint32_t x, y;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t dst_x0;
    uint32_t dst_y0;
    const uint32_t *src;

    if (g_rigel_zero_copy_video) {
        PAL_Video_Flip();
        return;
    }

    if (!g_rigel || !framebuffer || !pitch || !rigel_get_frame(g_rigel, &frame))
        return;

    /* When DIWSTRT/DIWSTOP are both 0 (cleared at reset), display_window_update
     * produces width=1, height=1 pointing to the VBLANK region.  Skip the PAL
     * VBLANK (26 lines) and fall back to the standard 320×256 active area. */
    if (frame.width < 16u || frame.height < 16u) {
        frame.pixels = (const rigel_u32 *)((const uint8_t *)frame.pixels
                                           + 26u * frame.pitch);
        frame.width  = 320u;
        frame.height = 256u;
    }

    /* Guard: VBL copper writes (e.g. DIWSTRT=0xffff before BPLCON0 depth=0) can
     * produce out-of-range visible_y_start, causing rigel_get_frame to return a
     * nonsensical height via uint wrap.  Skip the frame to avoid reading past the
     * frame buffer and corrupting the SDL surface. */
    if (!frame.pixels || frame.width > 1024u || frame.height > 512u)
        return;

    stride = pitch / 2u;
    src = frame.pixels;

    if (!src || frame.width == 0u || frame.height == 0u)
        return;

    uint32_t scale_x = fb_width / frame.width;
    uint32_t scale_y = fb_height / frame.height;
    if (scale_x < 1u) scale_x = 1u;
    if (scale_y < 1u) scale_y = 1u;

    width = frame.width;
    if (width > fb_width / scale_x)
        width = fb_width / scale_x;
    height = frame.height;
    if (height > fb_height / scale_y)
        height = fb_height / scale_y;

    if (width == 0u || height == 0u)
        return;

    dst_x0 = (fb_width > width * scale_x) ? (fb_width - width * scale_x) / 2u : 0u;
    dst_y0 = (fb_height > height * scale_y) ? (fb_height - height * scale_y) / 2u : 0u;

    {
        uint16_t bg = machine_rgb8888_to_le565(src[0]);
        for (y = 0; y < fb_height; ++y) {
            uint16_t *drow = framebuffer + y * stride;
            for (x = 0; x < fb_width; ++x)
                drow[x] = bg;
        }
    }

    for (y = 0; y < height; ++y) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)src + y * frame.pitch);
        for (x = 0; x < width; ++x) {
            uint16_t pixel = machine_rgb8888_to_le565(row[x]);
            for (uint32_t sy = 0u; sy < scale_y; ++sy) {
                uint16_t *drow = framebuffer + (dst_y0 + y * scale_y + sy) * stride;
                for (uint32_t sx = 0u; sx < scale_x; ++sx)
                    drow[dst_x0 + x * scale_x + sx] = pixel;
            }
        }
    }

    PAL_Video_Flip();
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
        while (rigel_serial_tx_available(g_rigel) &&
               rigel_serial_pop_tx_byte(g_rigel, &byte))
        {
            if (!uart_host_send_byte(&g_machine.uart_host, byte))
                break;
        }

        while (uart_host_receive_byte(&g_machine.uart_host, &byte))
            rigel_serial_receive_byte(g_rigel, byte);
    }

    /* kprintf's log ring only drains here, strictly after Paula's TX FIFO
     * above — Paula's bytes always reach the mini-UART first. See
     * AI_context/issue_logging_miniuart.md. */
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

    for (port = 0u; port < 2u; ++port) {
        const BellatrixControllerPortState *state = &m->controller_ports.port[port];
        rigel_input_set_joydat(g_rigel, port, controller_port_joydat(state));
        rigel_input_set_fire(g_rigel, port, state->fire ? true : false);
        rigel_input_set_pot_button_x(g_rigel, port, state->button2 ? true : false);
        rigel_input_set_pot_button_y(g_rigel, port, state->button3 ? true : false);
    }
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

    next = rigel_get_next_deadline(g_rigel);
    if (next > now && (next - now) < q) q = next - now;

    next = rigel_get_next_bus_change(g_rigel);
    if (next > now && (next - now) < q) q = next - now;

    if (q == 0u)
        return RIGEL_MIN_QUANTUM;

    return q;
}

/* Execute one quantum step and update machine state. Returns event flags. */
static rigel_event_flags_t machine_quantum_step(BellatrixMachine *m, rigel_cycle_t cycles)
{
    rigel_step_result_t r;

    if (!m || !g_rigel || cycles == 0u) return 0;

    r = rigel_step(g_rigel, cycles);
    m->tick_count = (uint64_t)r.time;

    machine_rigel_trace_step(&r);
    machine_step_host_serial_rigel();
    machine_drain_serial_fallback_rigel();
    /* Refill CIA-A SDR from the keyboard queue every chipset advance.
     * post_chipset_step only runs from the multicore Core-1 loop; in
     * single-core the queue would otherwise drain one byte per *new*
     * keypress, delivering keystrokes several events late. */
    machine_keyboard_drain_rigel();

    if (r.events & RIGEL_EVENT_FRAME_READY) {
        g_machine.frame_counter++;
        machine_present_frame_from_rigel();
    }
    if (r.events & RIGEL_EVENT_IRQ_CHANGED)
        machine_publish_ipl(m, rigel_get_ipl(g_rigel));

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
        rigel_event_flags_t ev = machine_quantum_step(m, s_quantum);
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

    rigel_event_flags_t ev = machine_quantum_step(m, partial);
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

void bellatrix_machine_on_frame_ready(void)
{
    g_machine.frame_counter++;
    machine_present_frame_from_rigel();
}

void bellatrix_machine_on_ipl_changed(uint8_t ipl)
{
    machine_publish_ipl(&g_machine, ipl);
}

void bellatrix_machine_post_chipset_step(void)
{
    machine_step_host_serial_rigel();
    machine_drain_serial_fallback_rigel();
    machine_keyboard_drain_rigel();
}

void bellatrix_machine_sync_ipl(void)
{
    if (g_rigel)
        machine_publish_ipl(&g_machine, rigel_get_ipl(g_rigel));
}

uint32_t bellatrix_machine_recommended_cpu_quantum(uint32_t max_cycles)
{
    rigel_cycle_t now;
    rigel_cycle_t next;
    rigel_cycle_t cck_quantum = RIGEL_MAX_QUANTUM;
    uint64_t cpu_quantum;

    if (!g_rigel)
        return max_cycles != 0u ? max_cycles : 1u;

    now = rigel_get_time(g_rigel);

    next = rigel_get_next_deadline(g_rigel);
    if (next > now && (next - now) < cck_quantum)
        cck_quantum = next - now;

    next = rigel_get_next_bus_change(g_rigel);
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
