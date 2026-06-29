// src/machine/machine_rigel_trace.c
//
// RigelTrace state, all trace/log helpers, machine_rigel_trace_step(),
// machine_publish_ipl().

#include "machine/machine_rigel_internal.h"

#include "machine/memory/chip_ram.h"
#include "debug/cpu_pc.h"
#include "host/pal.h"
#include "support.h"

#include "rigel/rigel.h"
#include "rigel/rigel_bus.h"
#include "rigel/rigel_custom.h"
#include "rigel/rigel_denise_debug.h"
#include "rigel/rigel_irq.h"

#include <string.h>

#ifdef BELLATRIX_HARNESS
#include <stdio.h>
#include <stdlib.h>
#endif

/* --------------------------------------------------------------------------
 * Rigel trace — structured event log built from rigel_step results
 *
 * Enabled via BELLATRIX_RIGEL_TRACE=1 env var (harness) or by calling
 * bellatrix_machine_rigel_trace_enable(true) from debug code (bare metal).
 * BELLATRIX_RIGEL_TRACE_VERBOSE=1 additionally logs bus/DMA changes.
 * -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  last_ipl;
    uint16_t last_intreq;
    uint16_t last_intena;
    uint16_t last_dmacon;
    uint16_t last_bplcon0;
    uint32_t last_frame_width;
    uint32_t last_frame_height;
    uint64_t frame_count;
    bool     enabled;
    bool     verbose;
} RigelTrace;

static RigelTrace g_rtrace;
static int g_rigel_cia_trace   = -1;
static int g_rigel_floppy_trace = -1;

bool machine_rigel_rtrace_enabled(void)
{
    return g_rtrace.enabled;
}

void machine_rigel_log(const char *msg, void *opaque)
{
    static const char floppy_prefix[] = "[RIGEL-FLOPPY-DRIVE]";

    (void)opaque;
    if (!g_rtrace.enabled &&
        !(msg && strncmp(msg, floppy_prefix, sizeof(floppy_prefix) - 1u) == 0 &&
          machine_rigel_floppy_trace_enabled()))
        return;
    kprintf("[rigel] %s\n", msg);
}

void machine_rigel_log_event(const rigel_log_event_t *event, void *opaque)
{
    static int generic_event_trace = -1;

    (void)opaque;
    if (!g_rtrace.enabled || event == NULL)
        return;

    if (generic_event_trace < 0) {
#ifdef BELLATRIX_HARNESS
        const char *env = getenv("BELLATRIX_RIGEL_EVENT_TRACE");
        generic_event_trace = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
#else
        generic_event_trace = 0;
#endif
    }

    /* AUD0-3 timing trace — all 4 channels. See
     * AI_context/issue_paula_audio_timing_and_simd.md. CCK-stamped via
     * rigel_get_time(), read live at the moment the event fires (more
     * precise than r->time, which only reflects the end of a step batch). */
    switch (event->id) {
    case RIGEL_LOG_EVENT_AUDIO_PER_WRITE:
        kprintf("[RIGEL-AUDIO-PER] ch=%u value=%04x audper=%u cyc=%llu\n",
                (unsigned)event->fields[0], (unsigned)event->fields[1],
                (unsigned)event->fields[2],
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0));
        return;
    case RIGEL_LOG_EVENT_AUDIO_PERIOD:
        kprintf("[RIGEL-AUDIO-TICK] ch=%u audper=%u cyc=%llu\n",
                (unsigned)event->fields[0], (unsigned)event->fields[1],
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0));
        return;
    case RIGEL_LOG_EVENT_AUDIO_IRQ:
        kprintf("[RIGEL-AUDIO-IRQ] ch=%u intreq_bit=%04x cyc=%llu\n",
                (unsigned)event->fields[0], (unsigned)event->fields[1],
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0));
        return;
    case RIGEL_LOG_EVENT_AUDIO_RELOAD:
        kprintf("[RIGEL-AUDIO-RELOAD] ch=%u audlc=%06x audlen=%u cyc=%llu\n",
                (unsigned)event->fields[0], (unsigned)event->fields[1],
                (unsigned)event->fields[2],
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0));
        return;
    case RIGEL_LOG_EVENT_AUDIO_FETCH:
        kprintf("[RIGEL-AUDIO-FETCH] ch=%u addr=%06x word=%04x remaining=%u cyc=%llu\n",
                (unsigned)event->fields[0], (unsigned)event->fields[1],
                (unsigned)event->fields[2], (unsigned)event->fields[3],
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0));
        return;
    case RIGEL_LOG_EVENT_AUDIO_DAT_WRITE:
        kprintf("[RIGEL-AUDIO-DAT] ch=%u value=%04x cyc=%llu\n",
                (unsigned)event->fields[0], (unsigned)event->fields[1],
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0));
        return;
    default:
        break;
    }

    if (!generic_event_trace)
        return;

    kprintf("[RIGEL-EVENT] %s", event->name ? event->name : "unknown");
    for (rigel_u8 i = 0u; i < event->field_count && i < 16u; i++) {
        kprintf(" f%u=%08x", (unsigned)i, (unsigned)event->fields[i]);
    }
    kprintf("\n");
}

int machine_rigel_cia_trace_enabled(void)
{
    if (g_rigel_cia_trace >= 0)
        return g_rigel_cia_trace;

#ifdef BELLATRIX_HARNESS
    {
        const char *env = getenv("BELLATRIX_RIGEL_CIA_TRACE");
        g_rigel_cia_trace = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
#else
    g_rigel_cia_trace = 0;
#endif
    return g_rigel_cia_trace;
}

int machine_rigel_floppy_trace_enabled(void)
{
    if (g_rigel_floppy_trace >= 0)
        return g_rigel_floppy_trace;

#ifdef BELLATRIX_HARNESS
    {
        const char *env = getenv("BELLATRIX_RIGEL_FLOPPY_TRACE");
        g_rigel_floppy_trace = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
#else
    g_rigel_floppy_trace = 0;
#endif
    return g_rigel_floppy_trace;
}

void machine_rigel_trace_floppy(const char *reason, uint32_t pc,
                                uint8_t reg, uint8_t value)
{
    rigel_floppy_status_t df0;
    uint8_t prb;
    uint8_t pra;
    int mtr;
    int sel0;
    int step;
    int dir;
    int side;

    if (!g_rigel || !machine_rigel_floppy_trace_enabled())
        return;

    if (!rigel_floppy_get_status(g_rigel, RIGEL_FLOPPY_DRIVE_DF0, &df0))
        return;

    prb = rigel_cia_read(g_rigel, 1u, 0x1u);
    pra = rigel_cia_read(g_rigel, 0u, 0x0u);

    mtr  = (prb & 0x80u) == 0u;
    sel0 = (prb & 0x08u) == 0u;
    step = (prb & 0x01u) == 0u;
    dir  = (prb & 0x02u) != 0u;
    side = (prb & 0x04u) == 0u;

    kprintf("[RIGEL-FLOPPY] %s pc=%08x reg=%x val=%02x prb=%02x pra=%02x "
            "mtr=%d sel0=%d step=%d dir=%s side=%d "
            "df0_sel=%d motor=%d cyl=%u side=%u media=%d rdy=%d trk0=%d "
            "chg=%d wpro=%d dma=%d lines=/chg%d /wpro%d /trk0%d /rdy%d\n",
            reason ? reason : "?",
            (unsigned)pc,
            (unsigned)(reg & 0x0fu),
            (unsigned)value,
            (unsigned)prb,
            (unsigned)pra,
            mtr,
            sel0,
            step,
            dir ? "out" : "in",
            side,
            df0.selected ? 1 : 0,
            df0.motor_on ? 1 : 0,
            (unsigned)df0.cylinder,
            (unsigned)df0.side,
            df0.has_media ? 1 : 0,
            df0.ready ? 1 : 0,
            df0.track0 ? 1 : 0,
            df0.disk_changed ? 1 : 0,
            df0.write_protected ? 1 : 0,
            df0.dma_active ? 1 : 0,
            (pra & 0x04u) ? 1 : 0,
            (pra & 0x08u) ? 1 : 0,
            (pra & 0x10u) ? 1 : 0,
            (pra & 0x20u) ? 1 : 0);
}

int machine_rigel_blitter_trace_enabled(void)
{
#ifdef BELLATRIX_HARNESS
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("BELLATRIX_RIGEL_BLITTER_TRACE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return enabled;
#else
    return 0;
#endif
}

void machine_rigel_trace_init(void)
{
    /* Compile-time opt-in: -DBELLATRIX_RIGEL_TRACE_BUILD=1 enables the trace
     * unconditionally (bare-metal builds that want verbose output).
     * In harness builds, the BELLATRIX_RIGEL_TRACE env var takes precedence. */
#if defined(BELLATRIX_RIGEL_TRACE_BUILD) && BELLATRIX_RIGEL_TRACE_BUILD
    bool enabled = true;
    bool verbose = false;
#else
    bool enabled = false;
    bool verbose = false;
#endif
#ifdef BELLATRIX_HARNESS
    const char *e = getenv("BELLATRIX_RIGEL_TRACE");
    if (e != NULL) enabled = (e[0] != '\0' && e[0] != '0');
    const char *v = getenv("BELLATRIX_RIGEL_TRACE_VERBOSE");
    if (v != NULL) verbose = (v[0] != '\0' && v[0] != '0');
#endif
    g_rtrace.enabled      = enabled;
    g_rtrace.verbose      = verbose;
    g_rtrace.last_ipl     = 0;
    g_rtrace.last_intreq  = 0;
    g_rtrace.last_intena  = 0;
    g_rtrace.last_dmacon  = 0;
    g_rtrace.last_bplcon0 = 0;
    g_rtrace.last_frame_width = 0;
    g_rtrace.last_frame_height = 0;
    g_rtrace.frame_count  = 0;
}

void bellatrix_machine_rigel_trace_enable(bool enabled)
{
    g_rtrace.enabled = enabled;
}

static uint32_t machine_rigel_count_nonzero_words(uint32_t addr, uint32_t words)
{
    uint32_t count = 0u;

    if (!chip_ram_is_configured(&g_machine.memory))
        return 0u;

    for (uint32_t i = 0u; i < words; i++) {
        if (chip_ram_read16(&g_machine.memory, addr + i * 2u) != 0u)
            count++;
    }

    return count;
}

static uint32_t machine_rigel_estimate_plane_span(uint32_t width,
                                                  uint32_t height,
                                                  uint16_t modulo)
{
    uint32_t words_per_line = width / 16u;
    uint32_t row_bytes = words_per_line * 2u;
    int32_t stride = (int32_t)row_bytes + (int16_t)modulo;

    if (words_per_line == 0u || height == 0u)
        return 0u;

    if (stride <= 0)
        stride = (int32_t)row_bytes;

    return (uint32_t)stride * height;
}

#ifdef BELLATRIX_HARNESS
static void machine_rigel_maybe_dump_frame(const rigel_frame_t *frame)
{
    static int init;
    static int done;
    static int dump_palette;
    static uint64_t dump_frame;
    static const char *dump_path;

    if (!init) {
        const char *frame_env = getenv("BELLATRIX_RIGEL_DUMP_FRAME");
        const char *palette_env = getenv("BELLATRIX_RIGEL_DUMP_PALETTE");
        dump_path = getenv("BELLATRIX_RIGEL_DUMP_PPM");
        init = 1;
        if (frame_env && *frame_env)
            dump_frame = (uint64_t)strtoull(frame_env, NULL, 0);
        dump_palette = palette_env && *palette_env && *palette_env != '0';
    }

    if (done || !dump_frame || !dump_path || !*dump_path ||
            g_rtrace.frame_count < dump_frame)
        return;

    if (!frame || !frame->pixels || frame->width == 0u || frame->height == 0u) {
        kprintf("[RIGEL-DUMP] skipped frame=%llu frame_ptr=%p pixels=%p size=%ux%u\n",
                (unsigned long long)g_rtrace.frame_count,
                (const void *)frame,
                frame ? frame->pixels : NULL,
                frame ? (unsigned)frame->width : 0u,
                frame ? (unsigned)frame->height : 0u);
        done = 1;
        return;
    }

    FILE *fp = fopen(dump_path, "wb");
    if (!fp) {
        kprintf("[RIGEL-DUMP] failed to open %s\n", dump_path);
        done = 1;
        return;
    }

    fprintf(fp, "P6\n%u %u\n255\n", (unsigned)frame->width, (unsigned)frame->height);
    for (uint32_t y = 0; y < frame->height; y++) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)frame->pixels +
                                                 (uintptr_t)y * frame->pitch);
        for (uint32_t x = 0; x < frame->width; x++) {
            uint32_t rgba = row[x];
            fputc((int)((rgba >> 16) & 0xffu), fp);
            fputc((int)((rgba >> 8) & 0xffu), fp);
            fputc((int)(rgba & 0xffu), fp);
        }
    }

    fclose(fp);
    kprintf("[RIGEL-DUMP] frame=%llu rigel_frame=%llu wrote %s size=%ux%u\n",
            (unsigned long long)g_rtrace.frame_count,
            (unsigned long long)frame->frame_count,
            dump_path,
            (unsigned)frame->width,
            (unsigned)frame->height);
    if (dump_palette && g_rigel) {
        kprintf("[RIGEL-DUMP-REGS] bplcon0=%04x bplcon1=%04x bplcon2=%04x "
                "diw=%04x/%04x ddf=%04x/%04x mod=%04x/%04x\n",
                rigel_custom_read16(g_rigel, RIGEL_REG_BPLCON0),
                rigel_custom_read16(g_rigel, RIGEL_REG_BPLCON1),
                rigel_custom_read16(g_rigel, RIGEL_REG_BPLCON2),
                rigel_custom_read16(g_rigel, RIGEL_REG_DIWSTRT),
                rigel_custom_read16(g_rigel, RIGEL_REG_DIWSTOP),
                rigel_custom_read16(g_rigel, RIGEL_REG_DDFSTRT),
                rigel_custom_read16(g_rigel, RIGEL_REG_DDFSTOP),
                rigel_custom_read16(g_rigel, 0x108u),
                rigel_custom_read16(g_rigel, 0x10au));
        for (uint32_t i = 0; i < 32u; i += 8u) {
            kprintf("[RIGEL-DUMP-COLOR] %02u:%04x %02u:%04x %02u:%04x %02u:%04x "
                    "%02u:%04x %02u:%04x %02u:%04x %02u:%04x\n",
                    (unsigned)i + 0u,
                    rigel_custom_read16(g_rigel, RIGEL_REG_COLOR00 + (i + 0u) * 2u),
                    (unsigned)i + 1u,
                    rigel_custom_read16(g_rigel, RIGEL_REG_COLOR00 + (i + 1u) * 2u),
                    (unsigned)i + 2u,
                    rigel_custom_read16(g_rigel, RIGEL_REG_COLOR00 + (i + 2u) * 2u),
                    (unsigned)i + 3u,
                    rigel_custom_read16(g_rigel, RIGEL_REG_COLOR00 + (i + 3u) * 2u),
                    (unsigned)i + 4u,
                    rigel_custom_read16(g_rigel, RIGEL_REG_COLOR00 + (i + 4u) * 2u),
                    (unsigned)i + 5u,
                    rigel_custom_read16(g_rigel, RIGEL_REG_COLOR00 + (i + 5u) * 2u),
                    (unsigned)i + 6u,
                    rigel_custom_read16(g_rigel, RIGEL_REG_COLOR00 + (i + 6u) * 2u),
                    (unsigned)i + 7u,
                    rigel_custom_read16(g_rigel, RIGEL_REG_COLOR00 + (i + 7u) * 2u));
        }
    }
    done = 1;
}
#endif

void machine_rigel_trace_step(const rigel_step_result_t *r)
{
    uint8_t  ipl;
    uint16_t intreq;
    uint16_t intena;
    uint16_t dmacon;
    uint16_t bplcon0;

    if (!g_rtrace.enabled || !g_rigel)
        return;

    /* Track DMACON and BPLCON0 changes — unlimited (full boot coverage) */
    {
        dmacon  = rigel_custom_read16(g_rigel, 0x096u);
        bplcon0 = rigel_custom_read16(g_rigel, 0x100u);
        if (dmacon != g_rtrace.last_dmacon) {
            kprintf("[RIGEL-DMACON] %04x->%04x pc=%08x cyc=%llu\n",
                    (unsigned)g_rtrace.last_dmacon, (unsigned)dmacon,
                    (unsigned)bellatrix_debug_cpu_pc(),
                    (unsigned long long)r->time);
            g_rtrace.last_dmacon = dmacon;
        }
        if (bplcon0 != g_rtrace.last_bplcon0) {
            rigel_denise_debug_state_t dbg;
            uint16_t diwstrt = rigel_custom_read16(g_rigel, 0x08eu);
            uint16_t diwstop = rigel_custom_read16(g_rigel, 0x090u);
            uint16_t ddfstrt = rigel_custom_read16(g_rigel, 0x092u);
            uint16_t ddfstop = rigel_custom_read16(g_rigel, 0x094u);
            uint16_t bpl1mod = rigel_custom_read16(g_rigel, 0x108u);
            uint16_t bpl2mod = rigel_custom_read16(g_rigel, 0x10au);
            uint32_t bpl1pt = ((uint32_t)rigel_custom_read16(g_rigel, 0x0e0u) << 16) |
                              (uint32_t)rigel_custom_read16(g_rigel, 0x0e2u);
            uint32_t bpl2pt = ((uint32_t)rigel_custom_read16(g_rigel, 0x0e4u) << 16) |
                              (uint32_t)rigel_custom_read16(g_rigel, 0x0e6u);
            unsigned hpos = 0u;
            unsigned vpos = 0u;
            unsigned visible = 0u;
            if (rigel_denise_get_debug_state(g_rigel, &dbg)) {
                hpos = dbg.beam_hpos;
                vpos = dbg.beam_vpos;
                visible = dbg.visible_scanline ? 1u : 0u;
            }
            kprintf("[RIGEL-BPLCON0] %04x->%04x depth=%u beam=%03u,%03u vis=%u"
                    " diw=%04x/%04x ddf=%04x/%04x mod=%04x/%04x"
                    " bplpt=%06x/%06x pc=%08x cyc=%llu\n",
                    (unsigned)g_rtrace.last_bplcon0, (unsigned)bplcon0,
                    (unsigned)((bplcon0 >> 12) & 7u),
                    hpos, vpos, visible,
                    (unsigned)diwstrt, (unsigned)diwstop,
                    (unsigned)ddfstrt, (unsigned)ddfstop,
                    (unsigned)bpl1mod, (unsigned)bpl2mod,
                    (unsigned)(bpl1pt & 0x00ffffffu),
                    (unsigned)(bpl2pt & 0x00ffffffu),
                    (unsigned)bellatrix_debug_cpu_pc(),
                    (unsigned long long)r->time);
            g_rtrace.last_bplcon0 = bplcon0;
        }
    }

    if (r->events & RIGEL_EVENT_IRQ_CHANGED) {
        ipl    = rigel_get_ipl(g_rigel);
        intreq = rigel_get_intreq(g_rigel);
        intena = rigel_get_intena(g_rigel);

        if (ipl != g_rtrace.last_ipl) {
            kprintf("[RIGEL-IPL] %u->%u cyc=%llu INTREQ=%04x INTENA=%04x\n",
                    (unsigned)g_rtrace.last_ipl, (unsigned)ipl,
                    (unsigned long long)r->time,
                    (unsigned)intreq, (unsigned)intena);
            g_rtrace.last_ipl    = ipl;
            g_rtrace.last_intreq = intreq;
            g_rtrace.last_intena = intena;
        } else if (intreq != g_rtrace.last_intreq || intena != g_rtrace.last_intena) {
            kprintf("[RIGEL-IRQ] INTREQ %04x->%04x INTENA %04x->%04x ipl=%u cyc=%llu\n",
                    (unsigned)g_rtrace.last_intreq, (unsigned)intreq,
                    (unsigned)g_rtrace.last_intena, (unsigned)intena,
                    (unsigned)ipl,
                    (unsigned long long)r->time);
            g_rtrace.last_intreq = intreq;
            g_rtrace.last_intena = intena;
        }
    }

    if (r->events & RIGEL_EVENT_FRAME_READY) {
        rigel_frame_t frame;
        g_rtrace.frame_count++;
        if (rigel_get_frame(g_rigel, &frame)) {
#ifdef BELLATRIX_HARNESS
            machine_rigel_maybe_dump_frame(&frame);
#endif
            kprintf("[RIGEL-FRAME] N=%llu %ux%u flags=%02x ipl=%u intreq=%04x cyc=%llu\n",
                    (unsigned long long)g_rtrace.frame_count,
                    (unsigned)frame.width, (unsigned)frame.height,
                    (unsigned)frame.flags,
                    (unsigned)g_rtrace.last_ipl,
                    (unsigned)g_rtrace.last_intreq,
                    (unsigned long long)r->time);
            if (frame.width != g_rtrace.last_frame_width ||
                    frame.height != g_rtrace.last_frame_height ||
                    (g_rtrace.frame_count % 50u) == 0u) {
                uint16_t dm = rigel_custom_read16(g_rigel, 0x096u);
                uint16_t bp = rigel_custom_read16(g_rigel, 0x100u);
                uint16_t bplcon1 = rigel_custom_read16(g_rigel, 0x102u);
                uint16_t bplcon2 = rigel_custom_read16(g_rigel, 0x104u);
                uint16_t bplcon3 = rigel_custom_read16(g_rigel, 0x106u);
                uint16_t diwstrt = rigel_custom_read16(g_rigel, 0x08eu);
                uint16_t diwstop = rigel_custom_read16(g_rigel, 0x090u);
                uint16_t ddfstrt = rigel_custom_read16(g_rigel, 0x092u);
                uint16_t ddfstop = rigel_custom_read16(g_rigel, 0x094u);
                uint16_t diwhigh = rigel_custom_read16(g_rigel, 0x1e4u);
                rigel_denise_video_desc_t video;
                memset(&video, 0, sizeof(video));
                (void)rigel_denise_get_video_desc(g_rigel, &video);
                kprintf("[RIGEL-FRAME-VIDEO] N=%llu size=%ux%u bplcon0=%04x"
                        " bplcon1=%04x bplcon2=%04x bplcon3=%04x diwhigh=%04x"
                        " dmacon=%04x diw=%04x/%04x ddf=%04x/%04x"
                        " vis=%u..%u/%u..%u\n",
                        (unsigned long long)g_rtrace.frame_count,
                        (unsigned)frame.width, (unsigned)frame.height,
                        (unsigned)bp, (unsigned)bplcon1, (unsigned)bplcon2,
                        (unsigned)bplcon3, (unsigned)diwhigh, (unsigned)dm,
                        (unsigned)diwstrt, (unsigned)diwstop,
                        (unsigned)ddfstrt, (unsigned)ddfstop,
                        (unsigned)video.visible_x_start,
                        (unsigned)video.visible_x_stop,
                        (unsigned)video.visible_y_start,
                        (unsigned)video.visible_y_stop);
                g_rtrace.last_frame_width = frame.width;
                g_rtrace.last_frame_height = frame.height;
            }
            if (g_rtrace.frame_count <= 5u) {
                uint16_t bplcon0 = rigel_custom_read16(g_rigel, 0x100u);
                uint16_t dmacon  = rigel_custom_read16(g_rigel, 0x096u);
                uint16_t diwstrt = rigel_custom_read16(g_rigel, 0x08eu);
                uint16_t diwstop = rigel_custom_read16(g_rigel, 0x090u);
                uint16_t ddfstrt = rigel_custom_read16(g_rigel, 0x092u);
                uint16_t ddfstop = rigel_custom_read16(g_rigel, 0x094u);
                uint16_t bpl1mod = rigel_custom_read16(g_rigel, 0x108u);
                uint16_t bpl2mod = rigel_custom_read16(g_rigel, 0x10au);
                uint16_t color00 = rigel_custom_read16(g_rigel, 0x180u);
                uint16_t color01 = rigel_custom_read16(g_rigel, 0x182u);
                uint32_t bpl1pt = ((uint32_t)rigel_custom_read16(g_rigel, 0x0e0u) << 16) |
                                  (uint32_t)rigel_custom_read16(g_rigel, 0x0e2u);
                uint32_t bpl2pt = ((uint32_t)rigel_custom_read16(g_rigel, 0x0e4u) << 16) |
                                  (uint32_t)rigel_custom_read16(g_rigel, 0x0e6u);
                uint32_t bpl1span = machine_rigel_estimate_plane_span(frame.width,
                                                                      frame.height,
                                                                      bpl1mod);
                uint32_t bpl2span = machine_rigel_estimate_plane_span(frame.width,
                                                                      frame.height,
                                                                      bpl2mod);
                uint32_t bpl1start = (bpl1pt - bpl1span) & 0x00ffffffu;
                uint32_t bpl2start = (bpl2pt - bpl2span) & 0x00ffffffu;
                uint16_t bpl1w0 = chip_ram_read16(&g_machine.memory, bpl1pt);
                uint16_t bpl1w1 = chip_ram_read16(&g_machine.memory, bpl1pt + 2u);
                uint16_t bpl2w0 = chip_ram_read16(&g_machine.memory, bpl2pt);
                uint16_t bpl2w1 = chip_ram_read16(&g_machine.memory, bpl2pt + 2u);
                uint32_t bpl1nz = machine_rigel_count_nonzero_words(bpl1pt, 1024u);
                uint32_t bpl2nz = machine_rigel_count_nonzero_words(bpl2pt, 1024u);
                uint16_t bpl1startw0 = chip_ram_read16(&g_machine.memory, bpl1start);
                uint16_t bpl1startw1 = chip_ram_read16(&g_machine.memory, bpl1start + 2u);
                uint16_t bpl2startw0 = chip_ram_read16(&g_machine.memory, bpl2start);
                uint16_t bpl2startw1 = chip_ram_read16(&g_machine.memory, bpl2start + 2u);
                uint32_t bpl1startnz = machine_rigel_count_nonzero_words(bpl1start, bpl1span / 2u);
                uint32_t bpl2startnz = machine_rigel_count_nonzero_words(bpl2start, bpl2span / 2u);
                /* count dirty (composed) lines from delta bitmask */
                uint32_t dirty = 0u;
                unsigned bi;
                for (bi = 0u; bi < 5u; bi++) {
                    uint64_t w = frame.delta.dirty_lines[bi];
                    while (w) { dirty++; w &= w - 1u; }
                }
                kprintf("[RIGEL-CHIPSET] frame=%llu bplcon0=%04x depth=%u dmacon=%04x"
                        " diw=%04x/%04x ddf=%04x/%04x mod=%04x/%04x"
                        " bplpt=%06x/%06x bplw=%04x,%04x/%04x,%04x"
                        " bplnz=%u/%u bplstart=%06x/%06x"
                        " bplstartw=%04x,%04x/%04x,%04x"
                        " bplstartnz=%u/%u col=%04x/%04x dirty=%u\n",
                        (unsigned long long)g_rtrace.frame_count,
                        (unsigned)bplcon0, (unsigned)((bplcon0 >> 12) & 7u),
                        (unsigned)dmacon,
                        (unsigned)diwstrt, (unsigned)diwstop,
                        (unsigned)ddfstrt, (unsigned)ddfstop,
                        (unsigned)bpl1mod, (unsigned)bpl2mod,
                        (unsigned)(bpl1pt & 0x00ffffffu),
                        (unsigned)(bpl2pt & 0x00ffffffu),
                        (unsigned)bpl1w0, (unsigned)bpl1w1,
                        (unsigned)bpl2w0, (unsigned)bpl2w1,
                        (unsigned)bpl1nz, (unsigned)bpl2nz,
                        (unsigned)bpl1start, (unsigned)bpl2start,
                        (unsigned)bpl1startw0, (unsigned)bpl1startw1,
                        (unsigned)bpl2startw0, (unsigned)bpl2startw1,
                        (unsigned)bpl1startnz, (unsigned)bpl2startnz,
                        (unsigned)color00, (unsigned)color01,
                        (unsigned)dirty);
            }
            /* Every 50 frames log a chipset summary until bitplanes appear */
            if (g_rtrace.frame_count > 5u &&
                    (g_rtrace.frame_count % 50u) == 0u) {
                uint16_t dm = rigel_custom_read16(g_rigel, 0x096u);
                uint16_t bp = rigel_custom_read16(g_rigel, 0x100u);
                uint16_t diwstrt = rigel_custom_read16(g_rigel, 0x08eu);
                uint16_t diwstop = rigel_custom_read16(g_rigel, 0x090u);
                uint16_t ddfstrt = rigel_custom_read16(g_rigel, 0x092u);
                uint16_t ddfstop = rigel_custom_read16(g_rigel, 0x094u);
                uint16_t bpl1mod = rigel_custom_read16(g_rigel, 0x108u);
                uint16_t bpl2mod = rigel_custom_read16(g_rigel, 0x10au);
                uint16_t c0 = rigel_custom_read16(g_rigel, 0x180u);
                uint16_t c1 = rigel_custom_read16(g_rigel, 0x182u);
                uint32_t bpl1pt = ((uint32_t)rigel_custom_read16(g_rigel, 0x0e0u) << 16) |
                                  (uint32_t)rigel_custom_read16(g_rigel, 0x0e2u);
                uint32_t bpl2pt = ((uint32_t)rigel_custom_read16(g_rigel, 0x0e4u) << 16) |
                                  (uint32_t)rigel_custom_read16(g_rigel, 0x0e6u);
                uint32_t bpl1span = machine_rigel_estimate_plane_span(frame.width,
                                                                      frame.height,
                                                                      bpl1mod);
                uint32_t bpl2span = machine_rigel_estimate_plane_span(frame.width,
                                                                      frame.height,
                                                                      bpl2mod);
                uint32_t bpl1start = (bpl1pt - bpl1span) & 0x00ffffffu;
                uint32_t bpl2start = (bpl2pt - bpl2span) & 0x00ffffffu;
                uint16_t bpl1w0 = chip_ram_read16(&g_machine.memory, bpl1pt);
                uint16_t bpl1w1 = chip_ram_read16(&g_machine.memory, bpl1pt + 2u);
                uint16_t bpl2w0 = chip_ram_read16(&g_machine.memory, bpl2pt);
                uint16_t bpl2w1 = chip_ram_read16(&g_machine.memory, bpl2pt + 2u);
                uint32_t bpl1nz = machine_rigel_count_nonzero_words(bpl1pt, 1024u);
                uint32_t bpl2nz = machine_rigel_count_nonzero_words(bpl2pt, 1024u);
                uint16_t bpl1startw0 = chip_ram_read16(&g_machine.memory, bpl1start);
                uint16_t bpl1startw1 = chip_ram_read16(&g_machine.memory, bpl1start + 2u);
                uint16_t bpl2startw0 = chip_ram_read16(&g_machine.memory, bpl2start);
                uint16_t bpl2startw1 = chip_ram_read16(&g_machine.memory, bpl2start + 2u);
                uint32_t bpl1startnz = machine_rigel_count_nonzero_words(bpl1start, bpl1span / 2u);
                uint32_t bpl2startnz = machine_rigel_count_nonzero_words(bpl2start, bpl2span / 2u);
                kprintf("[RIGEL-CHIPSET] frame=%llu bplcon0=%04x depth=%u dmacon=%04x"
                        " diw=%04x/%04x ddf=%04x/%04x mod=%04x/%04x"
                        " bplpt=%06x/%06x bplw=%04x,%04x/%04x,%04x"
                        " bplnz=%u/%u bplstart=%06x/%06x"
                        " bplstartw=%04x,%04x/%04x,%04x"
                        " bplstartnz=%u/%u col=%04x/%04x\n",
                        (unsigned long long)g_rtrace.frame_count,
                        (unsigned)bp, (unsigned)((bp >> 12) & 7u),
                        (unsigned)dm,
                        (unsigned)diwstrt, (unsigned)diwstop,
                        (unsigned)ddfstrt, (unsigned)ddfstop,
                        (unsigned)bpl1mod, (unsigned)bpl2mod,
                        (unsigned)(bpl1pt & 0x00ffffffu),
                        (unsigned)(bpl2pt & 0x00ffffffu),
                        (unsigned)bpl1w0, (unsigned)bpl1w1,
                        (unsigned)bpl2w0, (unsigned)bpl2w1,
                        (unsigned)bpl1nz, (unsigned)bpl2nz,
                        (unsigned)bpl1start, (unsigned)bpl2start,
                        (unsigned)bpl1startw0, (unsigned)bpl1startw1,
                        (unsigned)bpl2startw0, (unsigned)bpl2startw1,
                        (unsigned)bpl1startnz, (unsigned)bpl2startnz,
                        (unsigned)c0, (unsigned)c1);
            }
            if (g_rtrace.frame_count <= 5u && frame.pixels &&
                    frame.width > 0u && frame.height > 0u) {
                uint32_t cx = frame.width  / 2u;
                uint32_t cy = frame.height / 2u;
                const uint32_t *tl = (const uint32_t *)frame.pixels;
                const uint32_t *mid = (const uint32_t *)
                    ((const uint8_t *)frame.pixels + cy * frame.pitch) + cx;
                kprintf("[RIGEL-PIXELS] frame=%llu tl=%08x mid=(%u,%u)=%08x\n",
                        (unsigned long long)g_rtrace.frame_count,
                        (unsigned)*tl, (unsigned)cx, (unsigned)cy,
                        (unsigned)*mid);
            }
        } else {
            kprintf("[RIGEL-FRAME] N=%llu (no data) ipl=%u cyc=%llu\n",
                    (unsigned long long)g_rtrace.frame_count,
                    (unsigned)g_rtrace.last_ipl,
                    (unsigned long long)r->time);
        }
    }

    if (r->events & RIGEL_EVENT_VBLANK) {
        /* Log only first few VBLANKs — after that it's every frame (50 Hz). */
        if (g_rtrace.frame_count <= 3u) {
            kprintf("[RIGEL-VBLK] frame=%llu ipl=%u intreq=%04x cyc=%llu\n",
                    (unsigned long long)g_rtrace.frame_count,
                    (unsigned)g_rtrace.last_ipl,
                    (unsigned)g_rtrace.last_intreq,
                    (unsigned long long)r->time);
        }
    }

    /* Log first copper trigger — critical for display pipeline */
    if (r->events & RIGEL_EVENT_COPPER) {
        static uint32_t copper_count = 0;
        copper_count++;
        if (copper_count <= 3u) {
            kprintf("[RIGEL-COPPER] trigger #%u frame=%llu cyc=%llu\n",
                    (unsigned)copper_count,
                    (unsigned long long)g_rtrace.frame_count,
                    (unsigned long long)r->time);
        }
    }

    if (g_rtrace.verbose) {
        if (r->events & RIGEL_EVENT_BLIT_DONE) {
            kprintf("[RIGEL-BLIT] done cyc=%llu\n", (unsigned long long)r->time);
        }

        if (r->events & (RIGEL_EVENT_BUS_CHANGED | RIGEL_EVENT_DMA_CHANGED)) {
            rigel_bus_state_t bus = rigel_get_bus_state(g_rigel);
            kprintf("[RIGEL-BUS] owner=%d dma=%02x stall=%d cyc=%llu\n",
                    (int)bus.owner, (unsigned)bus.active_dma,
                    (int)bus.cpu_would_stall,
                    (unsigned long long)r->time);
        }
    }
}

void machine_publish_ipl(BellatrixMachine *m, uint8_t ipl)
{
    if (ipl > 7u)
        ipl = 7u;

    if (g_rtrace.enabled && ipl != m->current_ipl) {
        uint32_t pc = bellatrix_debug_cpu_pc();
        uint32_t vec_off;
        uint32_t chip_val;

        if (ipl > m->current_ipl && ipl > 0u)
            vec_off = 0x60u + (uint32_t)ipl * 4u;
        else
            vec_off = 0x60u + (uint32_t)m->current_ipl * 4u;

        chip_val = bellatrix_chip_read32(&m->memory, vec_off);
        kprintf(ipl > m->current_ipl && ipl > 0u
                    ? "[IPL-RISE] %u->%u  vec=%03x chip[%03x]=%08x m68k_pc=%08x\n"
                    : "[IPL-DROP] %u->%u  vec=%03x chip[%03x]=%08x m68k_pc=%08x\n",
                (unsigned)m->current_ipl, (unsigned)ipl,
                (unsigned)vec_off, (unsigned)vec_off, (unsigned)chip_val,
                (unsigned)pc);
    }

    m->current_ipl = ipl;
    if (m->cpu_backend && m->cpu_backend->set_ipl)
        m->cpu_backend->set_ipl(m->cpu_backend->ctx, (int)ipl);
}
