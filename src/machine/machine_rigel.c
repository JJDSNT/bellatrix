// src/machine/machine_rigel.c

#include "machine/machine.h"

#include "machine/expansion.h"
#include "machine/expansions/lide_cdrom/lide_cdrom.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/bus/zorro3/zorro3.h"
#include "machine/bus/superbuster/superbuster.h"
#include "machine/memory/chip_ram.h"

#include "debug/btrace.h"
#include "debug/cpu_pc.h"
#include "debug/probe.h"
#include "host/pal.h"
#include "support.h"

#include <string.h>

#include "rigel/rigel.h"
#include "rigel/rigel_audio.h"
#include "rigel/rigel_bus.h"
#include "rigel/rigel_custom.h"
#include "rigel/rigel_denise_debug.h"
#include "rigel/rigel_denise_video.h"
#include "rigel/rigel_irq.h"

#ifdef BELLATRIX_HARNESS
#include <stdlib.h>
#endif

/* ---------------------------------------------------------------------------
 * Bare-metal libc stubs required by Rigel (snprintf, fprintf, stderr).
 * Not needed in harness builds where the host libc is linked.
 * vsnprintf is provided by bt_hal_raspi3.c when BTStack is enabled; only
 * define it here for non-BTStack bare-metal builds.
 * ------------------------------------------------------------------------- */
#ifndef BELLATRIX_HARNESS

#if !BELLATRIX_ENABLE_BTSTACK
typedef struct { char *buf; size_t size; size_t pos; } rigel_snprintf_ctx_t;
static void rigel_snprintf_putc(void *d, char c)
{
    rigel_snprintf_ctx_t *ctx = (rigel_snprintf_ctx_t *)d;
    if (ctx->pos + 1 < ctx->size) {
        ctx->buf[ctx->pos++] = c;
        ctx->buf[ctx->pos]   = 0;
    }
}
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
    rigel_snprintf_ctx_t ctx = {str, size, 0};
    if (size > 0) str[0] = 0;
    vkprintf_pc(rigel_snprintf_putc, &ctx, fmt, ap);
    return (int)ctx.pos;
}
#endif /* !BELLATRIX_ENABLE_BTSTACK */

int snprintf(char *str, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return n;
}

/* fprintf/stderr also provided by musashi_baremetal_stubs.c when Musashi is
 * built into the same image — only define here for the Emu68-JIT variant. */
#if !BELLATRIX_USE_MUSASHI_CPU
void *stderr = (void *)0;
int fprintf(void *f, const char *fmt, ...)
{
    va_list ap;
    (void)f;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    return 0;
}
#endif /* !BELLATRIX_USE_MUSASHI_CPU */

#endif /* !BELLATRIX_HARNESS */

/* Actual M68K PC captured in vectors.c before each bellatrix_bus_access call.
 * Defined here so both the harness and the Emu68 build share the same symbol. */
volatile uint32_t g_bellatrix_fault_pc = 0;

/* Most recent M68K PC captured in ExecutionLoop.c before bellatrix_machine_advance. */
volatile uint32_t g_bellatrix_exec_pc = 0;

extern uint16_t *framebuffer;
extern uint32_t pitch;
extern uint32_t fb_width;
extern uint32_t fb_height;

static BellatrixMachine g_machine;
static RigelContext *g_rigel;
static bool g_rigel_zero_copy_video;

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

static void machine_rigel_log(const char *msg, void *opaque)
{
    (void)opaque;
    if (!g_rtrace.enabled)
        return;
    kprintf("[rigel] %s\n", msg);
}

static void machine_rigel_trace_init(void)
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

static void machine_rigel_trace_step(const rigel_step_result_t *r)
{
    uint8_t  ipl;
    uint16_t intreq;
    uint16_t intena;
    uint16_t dmacon;
    uint16_t bplcon0;

    if (!g_rtrace.enabled || !g_rigel)
        return;

    /* Track DMACON and BPLCON0 changes — first 20 seconds (1000 frames) */
    if (g_rtrace.frame_count < 1000u) {
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
                uint16_t bplcon3 = rigel_custom_read16(g_rigel, 0x106u);
                uint16_t diwstrt = rigel_custom_read16(g_rigel, 0x08eu);
                uint16_t diwstop = rigel_custom_read16(g_rigel, 0x090u);
                uint16_t ddfstrt = rigel_custom_read16(g_rigel, 0x092u);
                uint16_t ddfstop = rigel_custom_read16(g_rigel, 0x094u);
                uint16_t diwhigh = rigel_custom_read16(g_rigel, 0x1e4u);
                kprintf("[RIGEL-FRAME-VIDEO] N=%llu size=%ux%u bplcon0=%04x"
                        " bplcon1=%04x bplcon3=%04x diwhigh=%04x"
                        " dmacon=%04x diw=%04x/%04x ddf=%04x/%04x\n",
                        (unsigned long long)g_rtrace.frame_count,
                        (unsigned)frame.width, (unsigned)frame.height,
                        (unsigned)bp, (unsigned)bplcon1, (unsigned)bplcon3,
                        (unsigned)diwhigh, (unsigned)dm,
                        (unsigned)diwstrt, (unsigned)diwstop,
                        (unsigned)ddfstrt, (unsigned)ddfstop);
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
                /* count dirty (composed) lines from delta bitmask */
                uint32_t dirty = 0u;
                unsigned bi;
                for (bi = 0u; bi < 5u; bi++) {
                    uint64_t w = frame.delta.dirty_lines[bi];
                    while (w) { dirty++; w &= w - 1u; }
                }
                kprintf("[RIGEL-CHIPSET] frame=%llu bplcon0=%04x depth=%u dmacon=%04x"
                        " diw=%04x/%04x ddf=%04x/%04x mod=%04x/%04x"
                        " bplpt=%06x/%06x col=%04x/%04x dirty=%u\n",
                        (unsigned long long)g_rtrace.frame_count,
                        (unsigned)bplcon0, (unsigned)((bplcon0 >> 12) & 7u),
                        (unsigned)dmacon,
                        (unsigned)diwstrt, (unsigned)diwstop,
                        (unsigned)ddfstrt, (unsigned)ddfstop,
                        (unsigned)bpl1mod, (unsigned)bpl2mod,
                        (unsigned)(bpl1pt & 0x00ffffffu),
                        (unsigned)(bpl2pt & 0x00ffffffu),
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
                uint16_t c0 = rigel_custom_read16(g_rigel, 0x180u);
                kprintf("[RIGEL-CHIPSET] frame=%llu bplcon0=%04x depth=%u dmacon=%04x"
                        " diw=%04x/%04x ddf=%04x/%04x col00=%04x\n",
                        (unsigned long long)g_rtrace.frame_count,
                        (unsigned)bp, (unsigned)((bp >> 12) & 7u),
                        (unsigned)dm,
                        (unsigned)diwstrt, (unsigned)diwstop,
                        (unsigned)ddfstrt, (unsigned)ddfstop,
                        (unsigned)c0);
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

static inline bool is_custom_addr(uint32_t addr)
{
    return (addr >= 0x00dff000u && addr <= 0x00dfffffu);
}

static inline bool is_cia_a_addr(uint32_t addr)
{
    return (addr & 1u) && (addr >= 0x00bfe001u && addr <= 0x00bfef01u);
}

static inline bool is_cia_b_addr(uint32_t addr)
{
    if (addr & 1u)
        return false;
    return (addr >= 0x00bfd000u && addr <= 0x00bfdf00u) ||
           (addr >= 0x00bfe000u && addr <= 0x00bfef00u);
}

static inline bool is_rtc_addr(uint32_t addr)
{
    return (addr >= 0x00dc0000u && addr <= 0x00dcffffu);
}

static inline bool is_autoconfig_addr(uint32_t addr)
{
    return (addr >= 0x00E80000u && addr <= 0x00E8FFFFu);
}

static inline bool is_z2_board_addr(uint32_t addr)
{
    return bellatrix_zorro2_in_board_window(addr);
}

static inline bool is_z3_board_addr(uint32_t addr)
{
    return bellatrix_zorro3_in_board_window(addr);
}

static inline bool is_superbuster_addr(uint32_t addr)
{
    return superbuster_owns(addr);
}

static void machine_debug_init(BellatrixMachine *m)
{
    BellatrixDebug *d = &m->debug;

    probe_init(&d->probe);
    btrace_init(&d->btrace);

    d->enable_probe = true;
    d->enable_btrace = true;
    d->dump_on_watchdog = true;
    d->dump_on_cpu_stop = true;
    d->dump_on_cpu_except = true;
    d->dump_on_ipl_change = false;
    d->probe_last_n = 128;
    d->btrace_last_n = 128;
    d->copper_max_insn = 32;
    d->btrace_reads = true;
    d->btrace_writes = true;
    d->btrace_only_chipset = true;
    d->btrace_addr_lo = 0x00dff000u;
    d->btrace_addr_hi = 0x00dfffffu;
}

static void machine_debug_reset(BellatrixMachine *m)
{
    probe_reset(&m->debug.probe);
    btrace_reset(&m->debug.btrace);
}

static inline uint32_t machine_cpu_pc(const BellatrixMachine *m)
{
    (void)m;
    return bellatrix_debug_cpu_pc();
}

uint32_t bellatrix_debug_cpu_pc(void)
{
    const BellatrixMachine *m = &g_machine;
    if (!m->cpu_backend || !m->cpu_backend->get_pc)
        return 0u;
    return m->cpu_backend->get_pc(m->cpu_backend->ctx);
}

static inline void machine_publish_ipl(BellatrixMachine *m, uint8_t ipl)
{
    if (ipl > 7u)
        ipl = 7u;

    m->current_ipl = ipl;
    if (m->cpu_backend && m->cpu_backend->set_ipl)
        m->cpu_backend->set_ipl(m->cpu_backend->ctx, (int)ipl);
}

static uint16_t rigel_chip_ram_read16(void *opaque, uint32_t addr)
{
    BellatrixMachine *m = (BellatrixMachine *)opaque;
    return bellatrix_chip_read16(&m->memory, addr);
}

static void rigel_trace_chip_write(uint32_t addr, uint16_t value)
{
    static uint32_t write_count;
    static uint32_t nonzero_count;

    if (!g_rtrace.enabled)
        return;

    addr &= 0x00ffffffu;
    if (addr < 0x012000u || addr >= 0x018000u)
        return;

    if (value != 0u) {
        if (nonzero_count < 160u) {
            kprintf("[RIGEL-CHIPRAM-W] addr=%06x value=%04x pc=%08x\n",
                    (unsigned)addr, (unsigned)value,
                    (unsigned)bellatrix_debug_cpu_pc());
        }
        nonzero_count++;
        return;
    }

    if (write_count < 40u) {
        kprintf("[RIGEL-CHIPRAM-W] addr=%06x value=%04x pc=%08x\n",
                (unsigned)addr, (unsigned)value,
                (unsigned)bellatrix_debug_cpu_pc());
    }
    write_count++;
}

static void rigel_chip_ram_write16(void *opaque, uint32_t addr, uint16_t value)
{
    BellatrixMachine *m = (BellatrixMachine *)opaque;
    rigel_trace_chip_write(addr, value);
    bellatrix_chip_write16(&m->memory, addr, value);
}

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

static void machine_present_frame_from_rigel(void)
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

static void machine_drain_serial_fallback_rigel(void)
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

static void machine_step_host_serial_rigel(void)
{
    uint8_t byte = 0;

    if (!g_rigel || !g_machine.uart_host.enabled)
        return;

    while (rigel_serial_tx_available(g_rigel) &&
           rigel_serial_pop_tx_byte(g_rigel, &byte))
    {
        if (!uart_host_send_byte(&g_machine.uart_host, byte))
            break;
    }

    while (uart_host_receive_byte(&g_machine.uart_host, &byte))
        rigel_serial_receive_byte(g_rigel, byte);
}

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

static void machine_sync_controller_ports_rigel(BellatrixMachine *m)
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

static void machine_mirror_cia_write(CIA *cia, uint8_t reg, uint8_t value)
{
    if (!cia)
        return;

    switch (reg & 0x0Fu) {
    case 0x0u: cia->pra = value; break;
    case 0x1u: cia->prb = value; break;
    case 0x2u: cia->ddra = value; break;
    case 0x3u: cia->ddrb = value; break;
    default: break;
    }
}

/* ---------------------------------------------------------------------------
 * Quantum-based Rigel advancement.
 *
 * Rigel owns the clock.  The JIT is an inexact tenant: bela_delta*8 tells
 * us approximately how many M68K master-clock cycles have been consumed, but
 * that number is never passed to rigel_step.  Instead we use it only to
 * decide WHEN to step Rigel.  Rigel is always advanced by the exact number
 * of cycles to the next event deadline — never by the JIT's approximation.
 *
 * Result: chipset timing (VBL, copper, DMA, audio) is cycle-exact.  CPU
 * speed is approximate, as on any emulator that lacks per-instruction timing.
 *
 * Bus accesses flush any partial accumulated cycles as a partial step so
 * that chipset register reads reflect the latest state.
 * ------------------------------------------------------------------------- */

/* Caps: one PAL frame worth of cycles; prevents starvation in cpu-only loops */
#define RIGEL_MAX_QUANTUM  71424u   /* 313 lines × 228 cycles — one PAL frame */
#define RIGEL_MIN_QUANTUM      8u   /* never step by zero                      */

static uint64_t      s_cpu_approx;  /* accumulated approximate CPU cycles      */
static rigel_cycle_t s_quantum;     /* cycles to next Rigel event (the budget) */

/* Compute cycles to the nearest upcoming Rigel event deadline. */
static rigel_cycle_t machine_next_quantum(void)
{
    rigel_cycle_t now  = rigel_get_time(g_rigel);
    rigel_cycle_t q    = RIGEL_MAX_QUANTUM;
    rigel_cycle_t next;

    next = rigel_get_next_deadline(g_rigel);
    if (next > now && (next - now) < q) q = next - now;

    next = rigel_get_next_bus_change(g_rigel);
    if (next > now && (next - now) < q) q = next - now;

    return q >= RIGEL_MIN_QUANTUM ? q : RIGEL_MIN_QUANTUM;
}

/* Execute one quantum step and update machine state. */
static void machine_quantum_step(BellatrixMachine *m, rigel_cycle_t cycles)
{
    rigel_step_result_t r;

    if (!m || !g_rigel || cycles == 0u) return;

    r = rigel_step(g_rigel, cycles);
    m->tick_count = (uint64_t)r.time;

    machine_rigel_trace_step(&r);
    machine_step_host_serial_rigel();
    machine_drain_serial_fallback_rigel();

    if (r.events & RIGEL_EVENT_FRAME_READY) {
        g_machine.agnus.beam.frame++;
        machine_present_frame_from_rigel();
    }
    if (r.events & RIGEL_EVENT_IRQ_CHANGED)
        machine_publish_ipl(m, rigel_get_ipl(g_rigel));
}

/*
 * Called with the approximate number of M68K cycles the JIT just consumed.
 * When that estimate reaches the current quantum, Rigel advances by the
 * EXACT quantum (not the estimate) and the next quantum is fetched.
 */
static inline void machine_step_components(BellatrixMachine *m, uint32_t approx)
{
    if (!m || !g_rigel) return;

    if (s_quantum == 0u)
        s_quantum = machine_next_quantum();

    s_cpu_approx += approx;

    while (s_cpu_approx >= s_quantum) {
        s_cpu_approx -= s_quantum;
        machine_quantum_step(m, s_quantum);
        s_quantum = machine_next_quantum();
    }
}

/*
 * Flush any accumulated partial cycles before a chipset bus access so that
 * register reads (VPOS, INTREQ, etc.) reflect the current beam position.
 * Advances Rigel by whatever is in s_cpu_approx as a partial step.
 */
static inline void machine_flush_for_bus(BellatrixMachine *m)
{
    if (!m || !g_rigel || s_cpu_approx == 0u) return;

    rigel_cycle_t partial = (rigel_cycle_t)s_cpu_approx;
    if (partial > s_quantum && s_quantum > 0u)
        partial = s_quantum;

    machine_quantum_step(m, partial);
    if (s_cpu_approx >= partial)
        s_cpu_approx -= partial;
    else
        s_cpu_approx = 0u;

    /* Recompute quantum from the new Rigel time. */
    s_quantum = machine_next_quantum();
}

BellatrixMachine *bellatrix_machine_get(void)
{
    return &g_machine;
}

BellatrixDebug *bellatrix_machine_debug(void)
{
    return &g_machine.debug;
}

void bellatrix_machine_attach_rom(const uint8_t *rom, uint32_t rom_size)
{
    bellatrix_memory_attach_rom(&g_machine.memory, rom, rom_size);
}

void bellatrix_machine_init(CpuBackend *cpu_backend)
{
    BellatrixMachine *m = &g_machine;
    rigel_config_t config;

    memset(m, 0, sizeof(*m));
    m->cpu_backend = cpu_backend;

    bellatrix_memory_init(&m->memory);
    bellatrix_keyboard_init(&m->keyboard);
    bellatrix_controller_ports_init(&m->controller_ports);
    uart_host_init(&m->uart_host);
    superbuster_init(&m->superbuster);
    bellatrix_zorro2_init();
    bellatrix_zorro3_init();
    machine_debug_init(m);

    machine_rigel_trace_init();
    s_cpu_approx = 0u;
    s_quantum    = 0u;

    memset(&config, 0, sizeof(config));
    config.clock_hz = 7093790u;
    config.chip_ram_size = (rigel_u32)m->memory.chip_ram_size;
    config.chip_ram.opaque = m;
    config.chip_ram.read16 = rigel_chip_ram_read16;
    config.chip_ram.write16 = rigel_chip_ram_write16;
    config.rtc_model = RIGEL_RTC_MODEL_OKI;
    config.rtc_time = 0;
    config.video_std = RIGEL_VIDEO_PAL;
    config.chipset_model = RIGEL_CHIPSET_ECS;
    config.log_fn = machine_rigel_log;
    config.log_opaque = NULL;
    config.pixel_format = RIGEL_PIXEL_RGBA8888;

    g_rigel_zero_copy_video = false;
    if (framebuffer && pitch && fb_width <= 1024u && fb_height <= 312u) {
        config.pixel_format = RIGEL_PIXEL_RGB565;
        config.framebuffer.pixels = framebuffer;
        config.framebuffer.width = fb_width;
        config.framebuffer.height = fb_height;
        config.framebuffer.pitch = pitch;
        config.framebuffer.format = RIGEL_PIXEL_RGB565;
        config.framebuffer.little_endian = true;
        g_rigel_zero_copy_video = true;
    }

    if (g_rigel) {
        rigel_destroy(g_rigel);
        g_rigel = NULL;
    }

    g_rigel = rigel_create(&config);
    if (!g_rigel)
        kprintf("[RIGEL] create failed\n");
    else if (g_rigel_zero_copy_video)
        kprintf("[RIGEL] zero-copy RGB565 video target %ux%u pitch=%u\n",
                (unsigned)fb_width, (unsigned)fb_height, (unsigned)pitch);

    if (g_rigel) {
        /* Bypass baud-rate timing so SERDAT writes appear immediately in the
         * TX FIFO.  Without this the guest stalls in a TBE polling loop every
         * time it uses the serial port (krnPutC, DiagROM, etc.). */
        rigel_serial_set_tx_instant(g_rigel, true);
        rigel_cia_write(g_rigel, 0u, 0x2u, 0x03u);
        machine_mirror_cia_write(&m->cia_a, 0x2u, 0x03u);
        /* Sync PRA from Rigel so harness_overlay() sees OVL=1 at boot.
         * Without this the mirror stays 0 and the ROM overlay is skipped,
         * causing the CPU to fetch reset vectors from uninitialised chip RAM. */
        machine_mirror_cia_write(&m->cia_a, 0x0u,
            (uint8_t)rigel_cia_read(g_rigel, 0u, 0x0u));
    }
    machine_sync_controller_ports_rigel(m);

    m->tick_count = 0;
    machine_publish_ipl(m, 0);
}

void bellatrix_machine_reset(void)
{
    BellatrixMachine *m = &g_machine;

    if (g_rigel)
        rigel_reset(g_rigel);

    bellatrix_keyboard_reset(&m->keyboard);
    bellatrix_controller_ports_reset(&m->controller_ports);
    machine_sync_controller_ports_rigel(m);

    superbuster_reset(&m->superbuster);
    bellatrix_zorro2_reset();
    bellatrix_zorro3_reset();
    machine_debug_reset(m);
    bellatrix_expansion_reset_all(m);

    if (g_rigel) {
        rigel_cia_write(g_rigel, 0u, 0x2u, 0x03u);
        machine_mirror_cia_write(&m->cia_a, 0x2u, 0x03u);
        machine_mirror_cia_write(&m->cia_a, 0x0u,
            (uint8_t)rigel_cia_read(g_rigel, 0u, 0x0u));
    }

    m->tick_count = 0;
    machine_publish_ipl(m, g_rigel ? rigel_get_ipl(g_rigel) : 0u);
}

void bellatrix_machine_advance(uint32_t ticks)
{
    machine_step_components(&g_machine, ticks);
    bellatrix_machine_sync_ipl();
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
    uint32_t quantum = max_cycles != 0u ? max_cycles : 1u;

    if (!g_rigel)
        return quantum;

    now = rigel_get_time(g_rigel);

    next = rigel_get_next_deadline(g_rigel);
    if (next > now && (next - now) < (rigel_cycle_t)quantum)
        quantum = (uint32_t)(next - now);

    next = rigel_get_next_bus_change(g_rigel);
    if (next > now && (next - now) < (rigel_cycle_t)quantum)
        quantum = (uint32_t)(next - now);

    return quantum != 0u ? quantum : 1u;
}

static uint32_t machine_custom_read(uint32_t addr, unsigned int size)
{
    uint16_t word;
    uint32_t reg = addr & 0x1FEu;

    if (!g_rigel)
        return 0u;

    word = rigel_custom_read16(g_rigel, reg);
    if (size == 1u)
        return (addr & 1u) ? (uint32_t)(word & 0x00FFu)
                           : (uint32_t)(word >> 8);
    return (uint32_t)word;
}

static void machine_custom_write(uint32_t addr, uint32_t value, unsigned int size)
{
    uint16_t word;
    uint32_t reg = addr & 0x1FEu;

    if (!g_rigel)
        return;

    if (size == 1u) {
        word = rigel_custom_read16(g_rigel, reg);
        if (addr & 1u)
            word = (uint16_t)((word & 0xFF00u) | (value & 0x00FFu));
        else
            word = (uint16_t)((word & 0x00FFu) | ((value & 0x00FFu) << 8));
    } else {
        word = (uint16_t)value;
    }

    if (g_rtrace.enabled &&
            (reg == 0x08eu || reg == 0x090u ||
             reg == 0x092u || reg == 0x094u ||
             reg == 0x096u || reg == 0x100u ||
             reg == 0x102u || reg == 0x106u ||
             reg == 0x1e4u)) {
        uint16_t before = rigel_custom_read16(g_rigel, reg);
        kprintf("[RIGEL-MMIO-W] reg=%03x before=%04x write=%04x size=%u pc=%08x cyc=%llu\n",
                (unsigned)reg,
                (unsigned)before,
                (unsigned)word,
                (unsigned)size,
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned long long)(g_rigel ? rigel_get_time(g_rigel) : 0u));
    }

    rigel_custom_write16(g_rigel, reg, word);
}

static uint32_t machine_dispatch_read(BellatrixMachine *m, uint32_t addr, unsigned int size)
{
    uint32_t value = 0xFFFFFFFFu;

    if (is_custom_addr(addr)) {
        value = machine_custom_read(addr, size);
    } else if (is_cia_a_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0Fu);
        value = rigel_cia_read(g_rigel, 0u, reg);
        machine_mirror_cia_write(&m->cia_a, reg, (uint8_t)value);
    } else if (is_cia_b_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0Fu);
        value = rigel_cia_read(g_rigel, 1u, reg);
        machine_mirror_cia_write(&m->cia_b, reg, (uint8_t)value);
    } else if (is_rtc_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 2) & 0x0Fu);
        value = rigel_rtc_read_reg(g_rigel, reg);
    } else if (is_autoconfig_addr(addr)) {
        switch (size) {
        case 1: value = bellatrix_zorro2_config_read8(addr); break;
        case 2: value = bellatrix_zorro2_config_read16(addr); break;
        case 4: value = bellatrix_zorro2_config_read32(addr); break;
        default: break;
        }
    } else if (is_z2_board_addr(addr)) {
        if (!bellatrix_expansion_bus_read(m, addr, size, &value)) {
            switch (size) {
            case 1: value = bellatrix_zorro2_board_read8(addr); break;
            case 2: value = bellatrix_zorro2_board_read16(addr); break;
            case 4: value = bellatrix_zorro2_board_read32(addr); break;
            default: break;
            }
        }
    } else if (is_z3_board_addr(addr)) {
        switch (size) {
        case 1: value = bellatrix_zorro3_board_read8(addr); break;
        case 2: value = bellatrix_zorro3_board_read16(addr); break;
        case 4: value = bellatrix_zorro3_board_read32(addr); break;
        default: break;
        }
    } else if (is_superbuster_addr(addr)) {
        value = superbuster_read8(&m->superbuster, addr);
    } else if (bellatrix_expansion_bus_read(m, addr, size, &value)) {
        return value;
    } else {
        switch (size) {
        case 1: value = bellatrix_mem_read8(&m->memory, addr); break;
        case 2: value = bellatrix_mem_read16(&m->memory, addr); break;
        default: break;
        }
    }

    return value;
}

static void machine_dispatch_write(BellatrixMachine *m, uint32_t addr, uint32_t value, unsigned int size)
{
    if (is_custom_addr(addr)) {
        machine_custom_write(addr, value, size);
    } else if (is_cia_a_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0Fu);
        rigel_cia_write(g_rigel, 0u, reg, (uint8_t)value);
        machine_mirror_cia_write(&m->cia_a, reg, (uint8_t)value);
    } else if (is_cia_b_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 8) & 0x0Fu);
        rigel_cia_write(g_rigel, 1u, reg, (uint8_t)value);
        machine_mirror_cia_write(&m->cia_b, reg, (uint8_t)value);
    } else if (is_rtc_addr(addr)) {
        uint8_t reg = (uint8_t)((addr >> 2) & 0x0Fu);
        rigel_rtc_write_reg(g_rigel, reg, (uint8_t)(value & 0x0Fu));
    } else if (is_autoconfig_addr(addr)) {
        switch (size) {
        case 1: bellatrix_zorro2_config_write8(addr, (uint8_t)value); break;
        case 2: bellatrix_zorro2_config_write16(addr, (uint16_t)value); break;
        case 4: bellatrix_zorro2_config_write32(addr, (uint32_t)value); break;
        default: break;
        }
    } else if (is_z2_board_addr(addr)) {
        if (!bellatrix_expansion_bus_write(m, addr, value, size)) {
            switch (size) {
            case 1: bellatrix_zorro2_board_write8(addr, (uint8_t)value); break;
            case 2: bellatrix_zorro2_board_write16(addr, (uint16_t)value); break;
            case 4: bellatrix_zorro2_board_write32(addr, (uint32_t)value); break;
            default: break;
            }
        }
    } else if (is_z3_board_addr(addr)) {
        switch (size) {
        case 1: bellatrix_zorro3_board_write8(addr, (uint8_t)value); break;
        case 2: bellatrix_zorro3_board_write16(addr, (uint16_t)value); break;
        case 4: bellatrix_zorro3_board_write32(addr, (uint32_t)value); break;
        default: break;
        }
    } else if (is_superbuster_addr(addr)) {
        superbuster_write8(&m->superbuster, addr, (uint8_t)value);
    } else if (bellatrix_expansion_bus_write(m, addr, value, size)) {
        return;
    } else {
        switch (size) {
        case 1: bellatrix_mem_write8(&m->memory, addr, (uint8_t)value); break;
        case 2: bellatrix_mem_write16(&m->memory, addr, (uint16_t)value); break;
        case 4: bellatrix_mem_write32(&m->memory, addr, (uint32_t)value); break;
        default: break;
        }
    }
}

uint32_t bellatrix_machine_read(uint32_t addr, unsigned int size)
{
    BellatrixMachine *m = &g_machine;

    /* One bus cycle elapsed; flush partial quantum so register reads
     * (VPOS, INTREQ, DMA owner) reflect the current Rigel state. */
    s_cpu_approx += 1u;
    machine_flush_for_bus(m);

    if (size == 4u) {
        uint32_t hi = machine_dispatch_read(m, addr, 2u);
        uint32_t lo = machine_dispatch_read(m, addr + 2u, 2u);
        return (hi << 16) | lo;
    }

    return machine_dispatch_read(m, addr, size);
}

void bellatrix_machine_write(uint32_t addr, uint32_t value, unsigned int size)
{
    BellatrixMachine *m = &g_machine;

    /* One bus cycle elapsed; flush so Rigel sees the write at the right
     * time and the post-write IPL reflects the updated register. */
    s_cpu_approx += 1u;
    machine_flush_for_bus(m);

    if (size == 4u) {
        machine_dispatch_write(m, addr, value >> 16, 2u);
        machine_dispatch_write(m, addr + 2u, value & 0xFFFFu, 2u);
    } else {
        machine_dispatch_write(m, addr, value, size);
    }

    bellatrix_machine_sync_ipl();
}

BellatrixMemory *bellatrix_machine_memory(void)
{
    return &g_machine.memory;
}

AgnusState *bellatrix_machine_agnus(void)
{
    return &g_machine.agnus;
}

Denise *bellatrix_machine_denise(void)
{
    return &g_machine.denise;
}

Paula *bellatrix_machine_paula(void)
{
    return &g_machine.paula;
}

CIA *bellatrix_machine_cia_a(void)
{
    return &g_machine.cia_a;
}

CIA *bellatrix_machine_cia_b(void)
{
    return &g_machine.cia_b;
}

RTCState *bellatrix_machine_rtc(void)
{
    return &g_machine.rtc;
}

void bellatrix_machine_floppy_update(void)
{
}

int bellatrix_machine_keyboard_rawkey(uint8_t rawkey, int pressed)
{
    if (!g_rigel)
        return 0;
    rigel_keyboard_inject(g_rigel, rawkey & 0x7Fu, pressed != 0);
    return 1;
}

void bellatrix_machine_controller_set_device(unsigned port, unsigned device)
{
    bellatrix_controller_port_set_device(&g_machine.controller_ports,
                                         port,
                                         (BellatrixControllerPortDevice)device);
    machine_sync_controller_ports_rigel(&g_machine);
}

void bellatrix_machine_mouse_button(unsigned port, unsigned button, int pressed)
{
    bellatrix_controller_port_set_mouse_button(&g_machine.controller_ports, port, button, pressed);
    machine_sync_controller_ports_rigel(&g_machine);
}

void bellatrix_machine_mouse_motion(unsigned port, int dx, int dy)
{
    bellatrix_controller_port_add_mouse_motion(&g_machine.controller_ports, port, dx, dy);
    machine_sync_controller_ports_rigel(&g_machine);
}

void bellatrix_machine_joystick_button(unsigned port, unsigned button, int pressed)
{
    bellatrix_controller_port_set_joystick_button(&g_machine.controller_ports, port, button, pressed);
    machine_sync_controller_ports_rigel(&g_machine);
}

void bellatrix_machine_joystick_direction(unsigned port, unsigned direction, int pressed)
{
    bellatrix_controller_port_set_joystick_direction(&g_machine.controller_ports, port, direction, pressed);
    machine_sync_controller_ports_rigel(&g_machine);
}

int bellatrix_machine_insert_df0_adf(const uint8_t *adf, uint32_t adf_size)
{
    if (!g_rigel || !adf || adf_size == 0u)
        return 0;
    if (rigel_floppy_insert(g_rigel, RIGEL_FLOPPY_DRIVE_DF0, adf, adf_size) != RIGEL_STATUS_OK)
        return 0;
    kprintf("[RIGEL] DF0 ADF inserted size=%u\n", (unsigned)adf_size);
    return 1;
}

void bellatrix_machine_eject_df0(void)
{
    if (!g_rigel)
        return;
    rigel_floppy_eject(g_rigel, RIGEL_FLOPPY_DRIVE_DF0);
    kprintf("[RIGEL] DF0 ejected\n");
}

int bellatrix_machine_insert_iso(const void *data, size_t size)
{
    BellatrixMachine *m = bellatrix_machine_get();
    lide_cdrom_register(m);
    return lide_cdrom_insert_iso(m, data, size);
}

int bellatrix_machine_attach_iso_fn(iso_read_fn fn, void *ctx, uint32_t sector_count)
{
    BellatrixMachine *m = bellatrix_machine_get();
    lide_cdrom_register(m);
    return lide_cdrom_attach_iso_fn(m, fn, ctx, sector_count);
}

void bellatrix_machine_eject_iso(void)
{
    lide_cdrom_eject(bellatrix_machine_get());
}

const char *bellatrix_machine_backend_name(void)
{
    return "rigel";
}

void bellatrix_machine_serial_receive_byte(uint8_t byte)
{
    if (!g_rigel)
        return;
    rigel_serial_receive_byte(g_rigel, byte);
}

int16_t bellatrix_machine_audio_left(void)
{
    rigel_audio_sample_t sample;

    if (!g_rigel)
        return 0;

    sample = rigel_get_audio_sample(g_rigel);
    return (int16_t)sample.left;
}

int16_t bellatrix_machine_audio_right(void)
{
    rigel_audio_sample_t sample;

    if (!g_rigel)
        return 0;

    sample = rigel_get_audio_sample(g_rigel);
    return (int16_t)sample.right;
}

int bellatrix_machine_serial_rx_pending(void)
{
    uint16_t serdatr;

    if (!g_rigel)
        return 0;

    serdatr = rigel_custom_read16(g_rigel, RIGEL_REG_SERDATR);
    return (serdatr & 0x4000u) ? 1 : 0;
}

void bellatrix_machine_btrace_log(uint32_t addr, uint32_t value,
                                  unsigned int size, uint8_t dir, uint8_t impl)
{
    BellatrixMachine *m = &g_machine;

    if (!m->debug.enable_btrace)
        return;

    btrace_log(&m->debug.btrace,
               (uint32_t)m->tick_count,
               machine_cpu_pc(m),
               addr, value, size, dir, impl);
}

void bellatrix_machine_btrace_set_filter(uint16_t filter)
{
    btrace_set_filter(&g_machine.debug.btrace, filter);
}
