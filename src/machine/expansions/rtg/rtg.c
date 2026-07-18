// Portable P96 linear-framebuffer board — see rtg.h and docs/rtg_design.md.

#include "machine/expansions/rtg/rtg.h"
#include "cpu/cpu_backend.h"
#include "cpu/direct_region.h"
#include "machine/bus/board_registry.h"
#include "machine/autoconfig/autoconfig.h"
#include "machine/expansion.h"
#include "machine/machine.h"
#include "support.h"

#include <string.h>

static _Alignas(BELLATRIX_DIRECT_PAGE_SIZE)
    uint8_t s_rtg_vram[BELLATRIX_RTG_VRAM_SIZE];
static uint8_t s_rtg_frame[1920u * 1080u * 4u];
static BellatrixRtgScanout s_rtg;
static uint8_t  s_rtg_config[AUTOCONFIG_DATA_SIZE];
static int      s_registered = 0;
static uint32_t s_direct_vram_base;
static struct {
    uint32_t dst, pitch, xy, wh, color, fmtmask, status;
    uint32_t src, src_pitch, src_xy, opcode;
} s_accel;

static void rtg_ensure_direct_vram(void);
static void rtg_unmap_direct_vram(void);

/* Boot ROM: DiagArea loader (CardLoader) + relocated bellatrix.card hunk,
 * built by cards/bellatrix.card/{Makefile,bootrom/} and embedded via
 * scripts/rom_to_c.py (see tools/harness/CMakeLists.txt, rtg_rom_data). */
extern const unsigned char g_rtg_rom_data[];
extern const size_t        g_rtg_rom_size;

/* ----------------------------------------------------------------------- */
/* register file                                                            */
/* ----------------------------------------------------------------------- */

static uint32_t rtg_reg_read(uint32_t reg)
{
    if (reg == RTG_REG_ACCEL_STATUS)
        return s_accel.status;
    if (reg == RTG_REG_ID) {
        static int s_id_read_seen = 0;
        if (!s_id_read_seen) {
            s_id_read_seen = 1;
            kprintf("[RTG] REG_ID probed (FindCard reached the board)\n");
        }
    }
    return bellatrix_rtg_scanout_reg_read(&s_rtg, reg);
}

static void rtg_reg_write(uint32_t reg, uint32_t value)
{
    static uint32_t probe_op, probe_count, probe_w, probe_h;
    {
        static uint32_t s_seen_mask;
        uint32_t bit = 1u << ((reg >> 2) & 31u);
        if (!(s_seen_mask & bit)) {
            s_seen_mask |= bit;
            kprintf("[RTG] first write reg=%02x value=%08x\n",
                    (unsigned)reg, (unsigned)value);
        }
    }
    if (reg == RTG_REG_ENABLE && value != s_rtg.enable) {
        kprintf("[RTG] enable=%u %ux%u fmt=%u bpr=%u\n",
                (unsigned)value, (unsigned)s_rtg.mode_w,
                (unsigned)s_rtg.mode_h, (unsigned)s_rtg.format,
                (unsigned)s_rtg.bytes_per_row);
    }
    if (reg == RTG_REG_ACCEL_DST) s_accel.dst = value;
    else if (reg == RTG_REG_ACCEL_PITCH) s_accel.pitch = value;
    else if (reg == RTG_REG_ACCEL_XY) s_accel.xy = value;
    else if (reg == RTG_REG_ACCEL_WH) s_accel.wh = value;
    else if (reg == RTG_REG_ACCEL_COLOR) s_accel.color = value;
    else if (reg == RTG_REG_ACCEL_FMTMASK) s_accel.fmtmask = value;
    else if (reg == RTG_REG_ACCEL_SRC) s_accel.src = value;
    else if (reg == RTG_REG_ACCEL_SRC_PITCH) s_accel.src_pitch = value;
    else if (reg == RTG_REG_ACCEL_SRC_XY) s_accel.src_xy = value;
    else if (reg == RTG_REG_ACCEL_OPCODE) s_accel.opcode = value;
    else if (reg == RTG_REG_ACCEL_COMMAND) {
        s_accel.status = 0u;
        if (value == RTG_ACCEL_FILLRECT) {
            static uint32_t fill_count;
            s_accel.status = (uint32_t)bellatrix_rtg_accel_fillrect(
                s_rtg_vram, BELLATRIX_RTG_VRAM_SIZE,
                s_accel.dst, s_accel.pitch,
                s_accel.xy >> 16, s_accel.xy & 0xffffu,
                s_accel.wh >> 16, s_accel.wh & 0xffffu,
                s_accel.color, s_accel.fmtmask >> 8,
                s_accel.fmtmask & 0xffu);
            if (s_accel.status && (++fill_count == 1u ||
                                   (fill_count & (fill_count - 1u)) == 0u))
                kprintf("[RTG-ACCEL] FillRect count=%u fmt=%u %ux%u handled=host\n",
                        (unsigned)fill_count,
                        (unsigned)(s_accel.fmtmask >> 8),
                        (unsigned)(s_accel.wh >> 16),
                        (unsigned)(s_accel.wh & 0xffffu));
        } else if (value == RTG_ACCEL_BLIT_COPY && s_accel.opcode == 0x0cu) {
            static uint32_t blit_count;
            s_accel.status = (uint32_t)bellatrix_rtg_accel_blit_copy(
                s_rtg_vram, BELLATRIX_RTG_VRAM_SIZE,
                s_accel.src, s_accel.src_pitch,
                s_accel.src_xy >> 16, s_accel.src_xy & 0xffffu,
                s_accel.dst, s_accel.pitch,
                s_accel.xy >> 16, s_accel.xy & 0xffffu,
                s_accel.wh >> 16, s_accel.wh & 0xffffu,
                s_accel.fmtmask >> 8);
            if (s_accel.status && (++blit_count == 1u ||
                                   (blit_count & (blit_count - 1u)) == 0u))
                kprintf("[RTG-ACCEL] BlitCopy count=%u fmt=%u %ux%u handled=host\n",
                        (unsigned)blit_count,
                        (unsigned)(s_accel.fmtmask >> 8),
                        (unsigned)(s_accel.wh >> 16),
                        (unsigned)(s_accel.wh & 0xffffu));
        }
    } else if (reg == RTG_REG_DEBUG) {
        static const char *const probe_names[] = {
            "unknown", "FillRect", "InvertRect", "BlitRect",
            "BlitTemplate", "BlitPattern", "DrawLine",
            "BlitRectNoMaskComplete"
        };
        if ((value & 0xff000000u) == 0xb7000000u) {
            probe_op = (value >> 20) & 0x0fu;
            probe_count = value & 0x000fffffu;
        } else if ((value & 0xff000000u) == 0xb8000000u) {
            probe_w = (value >> 12) & 0x0fffu;
            probe_h = value & 0x0fffu;
        } else if ((value & 0xff000000u) == 0xb9000000u) {
            const char *name = probe_op <
                sizeof(probe_names) / sizeof(probe_names[0]) ?
                probe_names[probe_op] : probe_names[0];
            kprintf("[RTG-PROBE] %s count=%u size=%ux%u fmt=%u arg=%02x "
                    "action=observed\n",
                    name, (unsigned)probe_count,
                    (unsigned)probe_w, (unsigned)probe_h,
                    (unsigned)((value >> 8) & 0xffffu),
                    (unsigned)(value & 0xffu));
        } else {
            kprintf("[RTG-DBG] %08x\n", (unsigned)value);
        }
    } else
        bellatrix_rtg_scanout_reg_write(&s_rtg, reg, value);
}

/* ----------------------------------------------------------------------- */
/* Zorro II byte ops (regs are 32-bit BE, assembled from byte lanes)        */
/* ----------------------------------------------------------------------- */

/* Byte write latch so 8/16-bit accesses to the 32-bit registers work:
 * bytes accumulate and the register commits on the write of its last
 * byte (offset % 4 == 3). Reads are stateless slices of the register. */
static uint32_t s_reg_latch[64];

static uint8_t rtg_read8(void *userdata, uint32_t offset)
{
    (void)userdata;
    if (offset >= BELLATRIX_RTG_VRAM_OFF) {
        return s_rtg.vram[offset - BELLATRIX_RTG_VRAM_OFF];
    }
    if (offset >= BELLATRIX_RTG_ROM_OFF) {
        static int s_rom_read_seen = 0;
        static int s_hunk_read_seen = 0;
        uint32_t rom_off = offset - BELLATRIX_RTG_ROM_OFF;
        if (!s_rom_read_seen) {
            s_rom_read_seen = 1;
            kprintf("[RTG] first ROM read at window offset 0x%x — DiagArea probed\n",
                    (unsigned)offset);
        }
        if (offset == BELLATRIX_RTG_CARD_OFF && !s_hunk_read_seen) {
            s_hunk_read_seen = 1;
            kprintf("[RTG] card hunk read starting — CardLoader relocating bellatrix.card\n");
        }
        return (rom_off < g_rtg_rom_size) ? g_rtg_rom_data[rom_off] : 0u;
    }
    {
        uint32_t reg  = offset & ~3u;
        uint32_t v    = rtg_reg_read(reg);
        unsigned lane = offset & 3u;
        return (uint8_t)(v >> ((3u - lane) * 8u));
    }
}

static void rtg_write8(void *userdata, uint32_t offset, uint8_t value)
{
    (void)userdata;
    if (offset >= BELLATRIX_RTG_VRAM_OFF) {
        s_rtg.vram[offset - BELLATRIX_RTG_VRAM_OFF] = value;
        return;
    }
    if (offset >= BELLATRIX_RTG_ROM_OFF) {
        return;  /* ROM: read-only */
    }
    {
        uint32_t reg  = offset & ~3u;
        unsigned lane = offset & 3u;
        unsigned slot = (reg >> 2) & 63u;
        uint32_t shift = (3u - lane) * 8u;
        s_reg_latch[slot] &= ~(0xFFu << shift);
        s_reg_latch[slot] |= ((uint32_t)value << shift);
        if (lane == 3u)
            rtg_reg_write(reg, s_reg_latch[slot]);
    }
}

static void rtg_reset(void *userdata)
{
    (void)userdata;
    rtg_unmap_direct_vram();
    memset(&s_accel, 0, sizeof(s_accel));
    bellatrix_rtg_scanout_reg_write(&s_rtg, RTG_REG_ENABLE, 0u);
    bellatrix_rtg_scanout_reg_write(&s_rtg, RTG_REG_PAN, 0u);
    bellatrix_rtg_scanout_reg_write(&s_rtg, RTG_REG_PAL_INDEX, 0u);
}

/* -----------------------------------------------------------------------
 * board_registry EXTERNAL Zorro III board
 *
 * RTG is a Zorro III board: its 8MB register+ROM+VRAM window is served per
 * access (EXTERNAL, map == NULL), so the walker only latches the guest-assigned
 * base into s_rtg_board.map_base. The window is decoded by the Super Buster
 * (superbuster_decode_z3 consults board_registry) and routed to the bus_ops
 * below via machine_rigel_bus.c's is_z3_board_addr -> expansion bus dispatch.
 * The descriptor is always linked in; enabled flips to 1 in the register call.
 * --------------------------------------------------------------------- */
static struct ExpansionBoard s_rtg_board = {
    .rom_file = s_rtg_config,
    .rom_size = BELLATRIX_RTG_WINDOW,
    .map_base = 0u,
    .is_z3    = 1u,
    .enabled  = 0u,
    .map      = NULL,
};
BELLATRIX_REGISTER_BOARD_Z3(s_rtg_board);

static uint32_t rtg_window_base(void) { return s_rtg_board.map_base; }

/* The register/ROM prefix remains side-effecting MMIO.  On the first VRAM
 * transaction, promote only the page-aligned framebuffer tail to a DIRECT
 * CPU region.  That first transaction still completes through the slow path;
 * subsequent accesses avoid the expansion bridge and byte-at-a-time loop. */
static void rtg_ensure_direct_vram(void)
{
    CpuBackend *backend;
    BellatrixDirectRegion region;
    uint32_t board_base;

    if (s_direct_vram_base != 0u)
        return;
    board_base = rtg_window_base();
    backend = cpu_backend_selected();
    if (board_base == 0u || !backend)
        return;

    region.guest_base = board_base + BELLATRIX_RTG_VRAM_OFF;
    region.size = BELLATRIX_RTG_VRAM_SIZE;
    region.host_base = s_rtg_vram;
    region.flags = BELLATRIX_DIRECT_READ | BELLATRIX_DIRECT_WRITE |
                   BELLATRIX_DIRECT_CACHEABLE;
    if (cpu_backend_map_direct(backend, &region) != 0) {
        kprintf("[RTG] warning: direct VRAM mapping failed; using MMIO path\n");
        return;
    }
    s_direct_vram_base = region.guest_base;
    kprintf("[RTG] direct VRAM mapped: %08x-%08x (%u KB)\n",
            (unsigned)region.guest_base,
            (unsigned)(region.guest_base + region.size - 1u),
            (unsigned)(region.size / 1024u));
}

static void rtg_unmap_direct_vram(void)
{
    CpuBackend *backend;

    if (s_direct_vram_base == 0u)
        return;
    backend = cpu_backend_selected();
    if (backend)
        (void)cpu_backend_unmap_direct(backend, s_direct_vram_base,
                                       BELLATRIX_RTG_VRAM_SIZE);
    s_direct_vram_base = 0u;
}

/* Per-access window serving (bus_ops). owns_address gates read/write, so both
 * assume addr lies in the window; bytes are assembled/split big-endian, which
 * feeds the byte-latch commit in rtg_write8. */
static int rtg_bus_owns(BellatrixExpansion *exp, uint32_t addr)
{
    uint32_t base = rtg_window_base();
    (void)exp;
    if (!base)
        return 0;
    return addr >= base && addr < base + BELLATRIX_RTG_WINDOW;
}

static uint32_t rtg_bus_read(BellatrixExpansion *exp, uint32_t addr,
                             unsigned int size)
{
    uint32_t off = addr - rtg_window_base();
    uint32_t v = 0u;
    unsigned i;
    (void)exp;
    if (off >= BELLATRIX_RTG_VRAM_OFF)
        rtg_ensure_direct_vram();
    for (i = 0; i < size; i++)
        v = (v << 8) | rtg_read8(NULL, off + i);
    return v;
}

static void rtg_bus_write(BellatrixExpansion *exp, uint32_t addr,
                          uint32_t value, unsigned int size)
{
    uint32_t off = addr - rtg_window_base();
    unsigned i;
    (void)exp;
    if (off >= BELLATRIX_RTG_VRAM_OFF)
        rtg_ensure_direct_vram();
    for (i = 0; i < size; i++)
        rtg_write8(NULL, off + i, (uint8_t)(value >> ((size - 1u - i) * 8u)));
}

static const BellatrixExpansionBusOps s_rtg_bus_ops = {
    .owns_address = rtg_bus_owns,
    .read         = rtg_bus_read,
    .write        = rtg_bus_write,
};

static void rtg_exp_reset(BellatrixExpansion *exp) { (void)exp; rtg_reset(NULL); }

static const BellatrixExpansionOps s_rtg_exp_ops = {
    .attach   = 0,
    .reset    = rtg_exp_reset,
    .shutdown = 0,
    .destroy  = 0,
};

int bellatrix_rtg_register(struct BellatrixMachine *m)
{
    uint8_t raw[AUTOCONFIG_ROM_BYTES];
    BellatrixExpansionDesc desc;

    rtg_unmap_direct_vram();
    memset(s_rtg_vram, 0, sizeof(s_rtg_vram));
    bellatrix_rtg_scanout_init(&s_rtg,
                               s_rtg_vram, sizeof(s_rtg_vram),
                               BELLATRIX_RTG_VRAM_OFF,
                               s_rtg_frame, sizeof(s_rtg_frame));
    memset(raw, 0, sizeof(raw));
    /* Zorro III board. The size in er_Type bits 2-0 is enough for our
     * fixed-size window; the Super Buster decode keys off the assigned base. */
    raw[0]  = (uint8_t)(AC_TYPE_Z3 | AC_TYPE_DIAGVALID | AC_SIZE_8MB);
    raw[1]  = BELLATRIX_RTG_PRODUCT;
    raw[4]  = (uint8_t)(BELLATRIX_RTG_MANUFACTURER >> 8);
    raw[5]  = (uint8_t)(BELLATRIX_RTG_MANUFACTURER & 0xFFu);
    raw[9]  = 0x02u;   /* serial */
    raw[10] = (uint8_t)(BELLATRIX_RTG_ROM_OFF >> 8);   /* InitDiagVec high */
    raw[11] = (uint8_t)(BELLATRIX_RTG_ROM_OFF & 0xFFu); /* InitDiagVec low  */
    autoconfig_build(s_rtg_config, raw);

    s_rtg_board.map_base = 0u;
    s_rtg_board.enabled  = 1u;

    /* Per-access serving via expansion.c bus_ops; no legacy zorro registry. */
    memset(&desc, 0, sizeof(desc));
    desc.id           = "bellatrix.rtg";
    desc.name         = "Bellatrix RTG";
    desc.kind         = BELLATRIX_EXPANSION_BOARD;
    desc.priority     = 80;
    desc.userdata     = &s_rtg;
    desc.bus_ops      = &s_rtg_bus_ops;
    desc.ops          = &s_rtg_exp_ops;

    if (bellatrix_expansion_register(m, &desc) != 0) {
        s_rtg_board.enabled = 0u;
        return -1;
    }
    s_registered = 1;
    kprintf("[RTG] board registered: Zorro III 8MB window, %u KB VRAM\n",
            (unsigned)(BELLATRIX_RTG_VRAM_SIZE / 1024u));
    return 0;
}

/* ----------------------------------------------------------------------- */
/* host-side frame access                                                   */
/* ----------------------------------------------------------------------- */

int bellatrix_rtg_active(void)
{
    return s_registered && bellatrix_rtg_scanout_active(&s_rtg);
}

void bellatrix_rtg_frame_tick(void)
{
    bellatrix_rtg_scanout_frame_tick(&s_rtg);
}

uint8_t *bellatrix_rtg_vram_ptr(uint32_t *base, uint32_t *size)
{
    uint32_t board_base = rtg_window_base();
    if (!s_registered || !board_base)
        return 0;
    if (base) *base = board_base + BELLATRIX_RTG_VRAM_OFF;
    if (size) *size = BELLATRIX_RTG_VRAM_SIZE;
    return s_rtg.vram;
}

int bellatrix_rtg_get_frame(BellatrixRtgFrame *out)
{
    if (!s_registered)
        return 0;
    return bellatrix_rtg_scanout_render(&s_rtg, out);
}
