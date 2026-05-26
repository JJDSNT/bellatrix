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
#include "rigel/rigel_denise_video.h"
#include "rigel/rigel_irq.h"

#ifdef BELLATRIX_HARNESS
#include <stdlib.h>
#endif

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
    bool enabled = false;
    bool verbose = false;
#ifdef BELLATRIX_HARNESS
    const char *e = getenv("BELLATRIX_RIGEL_TRACE");
    enabled = (e != NULL && e[0] != '\0' && e[0] != '0');
    const char *v = getenv("BELLATRIX_RIGEL_TRACE_VERBOSE");
    verbose = (v != NULL && v[0] != '\0' && v[0] != '0');
#endif
    g_rtrace.enabled     = enabled;
    g_rtrace.verbose     = verbose;
    g_rtrace.last_ipl    = 0;
    g_rtrace.last_intreq = 0;
    g_rtrace.last_intena = 0;
    g_rtrace.frame_count = 0;
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

    if (!g_rtrace.enabled || !g_rigel)
        return;

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

static void rigel_chip_ram_write16(void *opaque, uint32_t addr, uint16_t value)
{
    BellatrixMachine *m = (BellatrixMachine *)opaque;
    bellatrix_chip_write16(&m->memory, addr, value);
}

static uint16_t machine_rgb8888_to_le565(uint32_t rgba)
{
    uint8_t r8 = (uint8_t)(rgba >> 24);
    uint8_t g8 = (uint8_t)(rgba >> 16);
    uint8_t b8 = (uint8_t)(rgba >> 8);
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
    const uint32_t *src;

    if (!g_rigel || !framebuffer || !pitch || !rigel_get_frame(g_rigel, &frame))
        return;

    width = frame.width < fb_width ? frame.width : fb_width;
    height = frame.height < fb_height ? frame.height : fb_height;
    stride = pitch / 2u;
    src = frame.pixels;

    if (!src || width == 0u || height == 0u)
        return;

    for (y = 0; y < height; ++y) {
        uint16_t *dst = framebuffer + y * stride;
        const uint32_t *row = (const uint32_t *)((const uint8_t *)src + y * frame.pitch);
        for (x = 0; x < width; ++x)
            dst[x] = machine_rgb8888_to_le565(row[x]);
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

static inline void machine_step_components(BellatrixMachine *m, uint32_t ticks)
{
    rigel_step_result_t r;

    if (!m || !g_rigel || ticks == 0u)
        return;

    r = rigel_step(g_rigel, (rigel_cycle_t)ticks);
    m->tick_count = (uint64_t)r.time;

    machine_rigel_trace_step(&r);

    machine_step_host_serial_rigel();
    machine_drain_serial_fallback_rigel();

    if (r.events & RIGEL_EVENT_FRAME_READY)
        machine_present_frame_from_rigel();

    if (r.events & RIGEL_EVENT_IRQ_CHANGED)
        machine_publish_ipl(m, rigel_get_ipl(g_rigel));
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

    memset(&config, 0, sizeof(config));
    config.clock_hz = 7093790u;
    config.chip_ram_size = (rigel_u32)m->memory.chip_ram_size;
    config.chip_ram.opaque = m;
    config.chip_ram.read16 = rigel_chip_ram_read16;
    config.chip_ram.write16 = rigel_chip_ram_write16;
    config.rtc_model = RIGEL_RTC_MODEL_OKI;
    config.rtc_time = 0;
    config.log_fn = machine_rigel_log;
    config.log_opaque = NULL;

    if (g_rigel) {
        rigel_destroy(g_rigel);
        g_rigel = NULL;
    }

    g_rigel = rigel_create(&config);
    if (!g_rigel)
        kprintf("[RIGEL] create failed\n");

    if (g_rigel) {
        rigel_cia_write(g_rigel, 0u, 0x2u, 0x03u);
        machine_mirror_cia_write(&m->cia_a, 0x2u, 0x03u);
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
    }

    m->tick_count = 0;
    machine_publish_ipl(m, g_rigel ? rigel_get_ipl(g_rigel) : 0u);
}

void bellatrix_machine_advance(uint32_t ticks)
{
    machine_step_components(&g_machine, ticks);
}

void bellatrix_machine_sync_ipl(void)
{
    if (g_rigel)
        machine_publish_ipl(&g_machine, rigel_get_ipl(g_rigel));
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

    machine_step_components(m, 1u);

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

    machine_step_components(m, 1u);

    if (size == 4u) {
        machine_dispatch_write(m, addr, value >> 16, 2u);
        machine_dispatch_write(m, addr + 2u, value & 0xFFFFu, 2u);
        return;
    }

    machine_dispatch_write(m, addr, value, size);
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
