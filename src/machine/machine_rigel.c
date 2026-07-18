// src/machine/machine_rigel.c
//
// Bare-metal libc stubs, global definitions, lifecycle (init/reset/attach_rom),
// and public API (accessors, floppy, keyboard, input, ISO stubs, backend_name).

#include "machine/machine_rigel_internal.h"

#include "machine/expansion.h"
#include "machine/expansions/lide_cdrom/lide_cdrom.h"
#include "cpu/cpu_backend.h"
#include "machine/bus/board_registry.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/bus/zorro3/zorro3.h"
#include "machine/bus/superbuster/superbuster.h"
#include "machine/memory/chip_ram.h"

#include "audio/output.h"
#include "debug/btrace.h"
#include "debug/core_log.h"
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

/* Rigel internals — only for keyboard-path diagnostics (CIA-A SDR/ICR
 * snapshot in bellatrix_machine_keyboard_rawkey). */
#include "core/rigel_context.h"

#ifdef BELLATRIX_HARNESS
#include <stdio.h>
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
#else  /* BELLATRIX_ENABLE_BTSTACK */
/* Definition lives in bt_hal_raspi3.c; snprintf below still needs the
 * prototype. */
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
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

BellatrixMachine g_machine;
RigelContext    *g_rigel;
bool             g_rigel_zero_copy_video;

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
    audio_mixer_init(&m->audio_queue);
    bellatrix_audio_output_init();
    superbuster_init(&m->superbuster);
    bellatrix_zorro2_init();
    bellatrix_zorro3_init();
    machine_debug_init(m);

    machine_rigel_trace_init();
    s_cpu_approx = 0u;
    s_cpu_cck_rem = 0u;
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
    config.log_event_fn = machine_rigel_log_event;
    config.log_event_opaque = NULL;
    config.pixel_format = RIGEL_PIXEL_RGBA8888;
    /* Bypass baud-rate timing so SERDAT writes appear immediately in the TX
     * FIFO. Without this the guest stalls in a TBE polling loop (krnPutC). */
    config.serial.tx_instant = true;

#ifdef BELLATRIX_HARNESS
    /*
     * The host SDL window is 640 pixels wide by default, while Rigel can export
     * a wider raster when border pixels are included (WB1.3 currently reaches
     * 892 pixels).  A direct RGB565 target would clip the right side.  Use the
     * presenter path in the harness so it can scale the full frame to the SDL
     * framebuffer instead of cropping it.
     */
    g_rigel_zero_copy_video = false;
#else
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
#endif

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

    /* Present an empty DF0 with correct mechanical state from cycle 0.
     * rigel_create() leaves CIA-A ext_pra at reset default (all lines high);
     * the /CHNG change latch of the connected empty drive is only reflected
     * after a floppy/CIA line sync, which otherwise first happens at the
     * guest's first CIA-B PRB write. The harness always ejected explicitly;
     * bare-metal must match, or early Kickstart PRA reads see "disk present"
     * (ISSUE-0038). A later ADF insert from the launcher overrides this. */
    if (g_rigel)
        bellatrix_machine_eject_df0();

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
    audio_mixer_init(&m->audio_queue);
    bellatrix_audio_output_reset();

    superbuster_reset(&m->superbuster);
    /* Tear down DIRECT regions installed by self-registering boards and rewind
     * the Autoconfig walk. struct ExpansionBoard has no unmap() (Emu68's model
     * installs once), so removal is generic: every mapped board backs a DIRECT
     * region, so drop it through the backend by (map_base, rom_size). */
    {
        CpuBackend *backend = cpu_backend_selected();
        size_t i, n = bellatrix_board_count();
        for (i = 0; i < n; ++i) {
            struct ExpansionBoard *b = bellatrix_board_at(i);
            if (!b || !b->map_base)
                continue;
            if (backend)
                (void)cpu_backend_unmap_direct(backend, b->map_base,
                                               b->rom_size);
            b->map_base = 0u;
        }
        bellatrix_boards_autoconfig_reset();
    }
    m->memory.fast_ram_base = 0u;
    m->memory.fast_ram_configured = 0u;
    bellatrix_zorro2_reset();
    bellatrix_zorro3_reset();
    machine_debug_reset(m);
    bellatrix_expansion_reset_all(m);

    m->tick_count = 0;
    s_cpu_approx = 0u;
    s_cpu_cck_rem = 0u;
    s_quantum = machine_next_quantum();
    machine_publish_ipl(m, g_rigel ? rigel_get_ipl(g_rigel) : 0u);
}

struct RigelContext *bellatrix_machine_rigel_ctx(void)
{
    return g_rigel;
}

BellatrixMemory *bellatrix_machine_memory(void)
{
    return &g_machine.memory;
}

/* Keyboard → CIA-A handshake tracking.
 *
 * A real Amiga keyboard waits for the KDAT handshake pulse (ROM sets CRA
 * SPMODE=out, then back to input) before clocking out the next byte.  If we
 * refill SDR as soon as the ROM reads it, the ROM's own SDR write during the
 * handshake (cia_serial_write_sdr stores unconditionally) clobbers the
 * pending byte — observed on hardware as a lost Enter key-up (SDR=0x00).
 * Like the real 6500/1 controller, fall back to resending after ~143ms if no
 * handshake arrives, so a ROM that never handshakes cannot stall the queue. */
#define KBD_HANDSHAKE_TIMEOUT_CCK 507000u   /* ~143ms at 3.546895 MHz CCK */

static uint8_t  s_kbd_await_handshake;
static uint8_t  s_kbd_cra_spmode_out;
static uint64_t s_kbd_sent_tick;

void machine_keyboard_on_cia_cra_write(uint8_t value)
{
    uint8_t spmode_out = (value & 0x40u) ? 1u : 0u;

    /* Handshake completes on the SPMODE out→input transition. */
    if (s_kbd_cra_spmode_out && !spmode_out)
        s_kbd_await_handshake = 0u;
    s_kbd_cra_spmode_out = spmode_out;
}

/* Push the next queued keyboard wire byte into CIA-A once the previous one
 * was consumed (ROM read of SDR clears sdr_full) AND the ROM finished the
 * KDAT handshake (or the resend timeout expired).  A real Amiga keyboard
 * buffers keystrokes in its 6500/1 controller and respects both gates;
 * injecting straight into SDR while it is still full silently drops keys. */
void machine_keyboard_drain_rigel(void)
{
    BellatrixKeyboard *kbd = &g_machine.keyboard;
    CIA *cia_a;
    uint8_t byte;

    if (!g_rigel || kbd->count == 0u)
        return;

    cia_a = &g_rigel->chipset.cia[0];
    if (cia_a->sdr_full)
        return;

    if (s_kbd_await_handshake &&
        (g_machine.tick_count - s_kbd_sent_tick) < KBD_HANDSHAKE_TIMEOUT_CCK)
        return;

    byte = kbd->queue[kbd->head];
    kbd->head = (uint8_t)((kbd->head + 1u) % BELLATRIX_KEYBOARD_QUEUE_CAP);
    kbd->count--;

    cia_receive_sdr(cia_a, byte);
    XCORE_LOG("IO->CHIPSET", "kbd byte 0x%02x delivered to CIA-A SDR queue_remaining=%u",
              (unsigned)byte, (unsigned)kbd->count);
    s_kbd_await_handshake = 1u;
    s_kbd_sent_tick = g_machine.tick_count;
}

int bellatrix_machine_keyboard_rawkey(uint8_t rawkey, int pressed)
{
    const CIA *cia_a;
    int queued;

    if (!g_rigel)
        return 0;

    /* enqueue_raw stores the Amiga wire encoding (~(code<<1 | up), i.e. the
     * same transform rigel_keyboard_inject applies), so the drain feeds the
     * byte to cia_receive_sdr directly. */
    queued = bellatrix_keyboard_enqueue_raw(&g_machine.keyboard, rawkey, !!pressed);
    machine_keyboard_drain_rigel();

    cia_a = &g_rigel->chipset.cia[0];
    kprintf("[KBD] rawkey=0x%02x %s%s queue=%u sdr=0x%02x full=%u icr_data=0x%02x icr_mask=0x%02x intena=0x%04x\n",
            (unsigned)rawkey, pressed ? "down" : "up",
            queued ? "" : " DROPPED(queue-full)",
            (unsigned)g_machine.keyboard.count,
            (unsigned)cia_a->sdr,
            (unsigned)cia_a->sdr_full,
            (unsigned)cia_a->icr_data,
            (unsigned)cia_a->icr_mask,
            (unsigned)rigel_custom_read16(g_rigel, 0x01Cu));
    return queued;
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
    if (port > 1u || button > BELLATRIX_MOUSE_BUTTON_RIGHT)
        return;

    bellatrix_controller_port_set_mouse_button(&g_machine.controller_ports, port, button, pressed);
    machine_sync_controller_ports_rigel(&g_machine);
}

/* AmigaOS samples JOY0DAT/JOY1DAT once per VBL and recovers the move as a
 * signed 8-bit delta (new - old). If more than +-127 counts land on the
 * register within a single VBL, the OS misreads direction/magnitude and its
 * internal pointer position drifts from then on -- this is what made clicks
 * land away from the visible cursor. Cap how much of each request actually
 * reaches the register per frame and carry the rest over to the next one, so
 * the net change between two VBL samples always stays inside the range the
 * OS can recover exactly. */
#define MOUSE_MOTION_PER_FRAME_LIMIT 100

static int s_mouse_pending_dx[2];
static int s_mouse_pending_dy[2];
static int s_mouse_applied_dx[2];
static int s_mouse_applied_dy[2];

static int mouse_motion_drain(int *pending, int *applied_this_frame)
{
    int total = *applied_this_frame + *pending;
    int room_hi = MOUSE_MOTION_PER_FRAME_LIMIT - *applied_this_frame;
    int room_lo = -MOUSE_MOTION_PER_FRAME_LIMIT - *applied_this_frame;
    int apply = total;

    if (apply > room_hi)
        apply = room_hi;
    if (apply < room_lo)
        apply = room_lo;

    *pending -= apply;
    *applied_this_frame += apply;
    return apply;
}

void bellatrix_machine_mouse_motion(unsigned port, int dx, int dy)
{
    if (port > 1u)
        return;

    s_mouse_pending_dx[port] += dx;
    s_mouse_pending_dy[port] += dy;
}

void machine_mouse_frame_tick(void)
{
    unsigned port;

    /* New frame: fresh +-LIMIT budget. Drain into it right away so motion
     * queued from a single large/fast swipe keeps catching up every VBL even
     * if the host mouse has since gone still (no further motion() calls to
     * piggyback the drain on). */
    for (port = 0u; port < 2u; ++port) {
        int apply_dx, apply_dy;

        s_mouse_applied_dx[port] = 0;
        s_mouse_applied_dy[port] = 0;

        apply_dx = mouse_motion_drain(&s_mouse_pending_dx[port], &s_mouse_applied_dx[port]);
        apply_dy = mouse_motion_drain(&s_mouse_pending_dy[port], &s_mouse_applied_dy[port]);

        if (apply_dx == 0 && apply_dy == 0)
            continue;

        bellatrix_controller_port_add_mouse_motion(&g_machine.controller_ports, port, apply_dx, apply_dy);
        machine_sync_controller_ports_rigel(&g_machine);
    }
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

int bellatrix_machine_insert_df_adf(unsigned drive, const uint8_t *adf,
                                    uint32_t adf_size)
{
    if (!g_rigel || !adf || adf_size == 0u)
        return 0;
    if (drive > 3u)
        return 0;
    if (rigel_floppy_insert(g_rigel, (rigel_floppy_drive_id_t)drive, adf, adf_size) != RIGEL_STATUS_OK)
        return 0;
    kprintf("[RIGEL] DF%u ADF inserted size=%u\n", drive, (unsigned)adf_size);
    return 1;
}

int bellatrix_machine_insert_df0_adf(const uint8_t *adf, uint32_t adf_size)
{
    return bellatrix_machine_insert_df_adf(0u, adf, adf_size);
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

int bellatrix_machine_attach_hdf_fn(int (*read_fn)(void *ctx, uint32_t lba,
                                                   uint32_t count,
                                                   uint8_t *buf),
                                    void *ctx, uint32_t sector_count)
{
    BellatrixMachine *m = bellatrix_machine_get();
    lide_cdrom_register(m);
    return lide_hd_attach(m, read_fn, NULL, ctx, sector_count);
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
