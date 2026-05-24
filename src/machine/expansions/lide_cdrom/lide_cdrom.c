#include "machine/expansions/lide_cdrom/lide_cdrom.h"

#include "machine/expansions/lide_cdrom/ata_ide.h"
#include "machine/expansions/lide_cdrom/atapi_cdrom.h"
#include "machine/autoconfig/autoconfig.h"
#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/expansion.h"
#include "machine/machine.h"

#include "support.h"

#include <stdlib.h>
#include <string.h>

/* Embedded ROM — generated at build time by scripts/rom_to_c.py */
extern const unsigned char g_lide_rom_data[];
extern const size_t        g_lide_rom_size;

/* -----------------------------------------------------------------------
 * lide.rom address-space mapping
 *
 * File layout (make_lide_rom.py):
 *   [0x0000..0x0003]  "LIV2" raw header
 *   [0x0004..0x07FF]  nibble-encoded bootldr (2 file bytes per logical byte)
 *   [0x0800..0x0FFF]  0xFF fill
 *   [0x1000..0x7FFF]  lide.device Hunk binary (raw, contiguous)
 *
 * Board address regions:
 *   [0x0000..0x0003]  raw LIV2 header
 *   [0x0004..0x0FFF]  NIBBLEWIDE: stride-4 board -> stride-2 file.
 *                     AROS reads high nibble at board[off*4], low at board[off*4+2];
 *                     we map those to file[4+off*2] and file[4+off*2+1].
 *   [0x1000..0x1FFF]  ATA registers (CS1=0x1000, stride=0x200) -- not ROM.
 *   [0x2000..0x7FFF]  BYTEWIDE device binary: board[0x2000+N*2] -> file[0x1000+N].
 *                     Odd board addresses return 0xFF.
 *
 * bootldr is assembled -DBYTEWIDE: InitHandle skips the x4 shift and reads the
 * device with stride 2 from DRIVEROFFSET=$2000.  So board $2000 = file $1000 =
 * lide.device[0] = first byte of HUNK_HEADER ($00/$00/$03/$F3). ✓
 * --------------------------------------------------------------------- */

#define RIPPLE_ROM_HDR_END      0x0004u   /* raw LIV2 header */
#define RIPPLE_ROM_NIBBLE_END   0x1000u   /* nibble region / ATA CS1 begin */
#define RIPPLE_ROM_DEVICE_BOARD 0x2000u   /* BYTEWIDE device start (board) */
#define RIPPLE_ROM_DEVICE_FILE  0x1000u   /* device start (file offset) */

static uint8_t ripple_rom_byte(const uint8_t *rom, size_t rom_size, uint32_t off)
{
    /* Raw LIV2 header */
    if (off < RIPPLE_ROM_HDR_END)
        return (off < rom_size) ? rom[off] : 0xFFu;

    /* NIBBLEWIDE bootldr: high nibble at phase 0, low nibble at phase 2 */
    if (off < RIPPLE_ROM_NIBBLE_END) {
        uint32_t rel   = off - RIPPLE_ROM_HDR_END;
        uint32_t phase = rel & 3u;
        uint32_t nidx  = rel >> 2;
        uint32_t fidx;
        if      (phase == 0u) fidx = RIPPLE_ROM_HDR_END + nidx * 2u;
        else if (phase == 2u) fidx = RIPPLE_ROM_HDR_END + nidx * 2u + 1u;
        else                  return 0xFFu;
        return (fidx < rom_size) ? rom[fidx] : 0xFFu;
    }

    /* ATA register window (0x1000..0x1FFF): caller routes to ATA layer */
    if (off < RIPPLE_ROM_DEVICE_BOARD)
        return 0xFFu;

    /* BYTEWIDE device binary: even board addresses only */
    if (off & 1u) return 0xFFu;
    {
        uint32_t fidx = RIPPLE_ROM_DEVICE_FILE + (off - RIPPLE_ROM_DEVICE_BOARD) / 2u;
        return (fidx < rom_size) ? rom[fidx] : 0xFFu;
    }
}

/* -----------------------------------------------------------------------
 * OAHR RIPPLE board identity
 * --------------------------------------------------------------------- */

#define RIPPLE_MFR_HI    0x14u           /* manufacturer 0x144A high byte */
#define RIPPLE_MFR_LO    0x4Au           /* manufacturer 0x144A low byte  */
#define RIPPLE_PROD      0x07u           /* RIPPLE_PROD_ID                */

/*
 * Board offsets on a configured RIPPLE board:
 *   CS1 (primary channel) = board_base + 0x1000
 *   Register stride       = 0x200
 */
#define RIPPLE_CS1_OFFSET  0x1000u
#define RIPPLE_NEXT_REG    0x0200u

/* -----------------------------------------------------------------------
 * ISO media backend
 * --------------------------------------------------------------------- */

typedef struct IsoMedia {
    const uint8_t   *mem_data;     /* in-memory image (may be NULL) */
    size_t           mem_size;
    iso_read_fn      fn;           /* multi-sector callback (may be NULL) */
    void            *fn_ctx;
    uint32_t         sector_count;
    int              present;
} IsoMedia;

static int iso_present(void *ctx)
{
    return ((IsoMedia *)ctx)->present;
}

static int iso_read_sectors(void *ctx, uint32_t lba, uint32_t count, uint8_t *buf)
{
    IsoMedia *m = (IsoMedia *)ctx;

    if (!m->present) return -1;

    if (m->fn)
        return m->fn(m->fn_ctx, lba, count, buf) ? 0 : -1;

    if (m->mem_data) {
        uint32_t i;
        for (i = 0; i < count; i++) {
            size_t off = (size_t)(lba + i) * ATAPI_SECTOR_SIZE;
            if (off + ATAPI_SECTOR_SIZE > m->mem_size) return -1;
            memcpy(buf + i * ATAPI_SECTOR_SIZE, m->mem_data + off, ATAPI_SECTOR_SIZE);
        }
        return 0;
    }

    return -1;
}

static uint32_t iso_sector_count(void *ctx)
{
    return ((IsoMedia *)ctx)->sector_count;
}

static const AtapiMediaOps g_iso_media_ops = {
    .present      = iso_present,
    .read_sectors = iso_read_sectors,
    .sector_count = iso_sector_count,
};

/* -----------------------------------------------------------------------
 * Expansion state
 * --------------------------------------------------------------------- */

typedef struct LideCdromState {
    AtaIdeChannel    ide;
    AtapiCdromState  atapi;
    IsoMedia         media;

    /* Zorro 2 config data */
    uint8_t          config_data[AUTOCONFIG_DATA_SIZE];

    /* Board ROM — points to embedded g_lide_rom_data (no ownership) */
    const uint8_t   *rom;
    size_t           rom_size;

    BellatrixZorro2BoardDesc board_desc;
} LideCdromState;

/* Global singleton (one CD-ROM expansion per machine for now) */
static LideCdromState *g_state;

/* -----------------------------------------------------------------------
 * Zorro 2 board ops
 * --------------------------------------------------------------------- */

static uint8_t ripple_read8(void *ud, uint32_t offset)
{
    LideCdromState *s = (LideCdromState *)ud;
    int reg;

    /* ATA registers at CS1 (0x1000..0x1FFF) take priority */
    reg = ata_ide_offset_to_reg(offset);
    if (reg >= 0)
        return ata_ide_read8(&s->ide, (uint8_t)reg);

    /* ROM: header, nibble-bootldr, and BYTEWIDE device section */
    if (s->rom)
        return ripple_rom_byte(s->rom, s->rom_size, offset);

    return 0xFFu;
}

static void ripple_write8(void *ud, uint32_t offset, uint8_t value)
{
    LideCdromState *s = (LideCdromState *)ud;
    int reg;

    reg = ata_ide_offset_to_reg(offset);
    if (reg >= 0)
        ata_ide_write8(&s->ide, (uint8_t)reg, value);
}

static void ripple_reset(void *ud)
{
    LideCdromState *s = (LideCdromState *)ud;
    ata_ide_channel_reset(&s->ide);
    atapi_cdrom_reset(&s->atapi);
}

static void ripple_destroy(void *ud)
{
    (void)ud;   /* lifetime managed by lide_cdrom_register caller */
}

static const BellatrixZorro2BoardOps g_ripple_ops = {
    .reset   = ripple_reset,
    .destroy = ripple_destroy,
    .read8   = ripple_read8,
    .write8  = ripple_write8,
};

/* -----------------------------------------------------------------------
 * Expansion bus ops (dispatched by expansion.c)
 * --------------------------------------------------------------------- */

static int ripple_bus_owns(BellatrixExpansion *exp, uint32_t addr)
{
    (void)exp;
    /* owned if address falls in the configured Z2 board window */
    return bellatrix_zorro2_in_board_window(addr) ||
           bellatrix_zorro2_in_config_window(addr);
}

static uint32_t ripple_bus_read(BellatrixExpansion *exp,
                                uint32_t addr, unsigned int size)
{
    (void)exp;

    if (bellatrix_zorro2_in_config_window(addr)) {
        switch (size) {
        case 1: return bellatrix_zorro2_config_read8(addr);
        case 2: return bellatrix_zorro2_config_read16(addr);
        case 4: return bellatrix_zorro2_config_read32(addr);
        default: return 0xFFFFFFFFu;
        }
    }

    if (bellatrix_zorro2_in_board_window(addr)) {
        LideCdromState *s = (LideCdromState *)exp->desc.userdata;
        uint32_t base     = bellatrix_zorro2_board_base("lide.cdrom");
        uint32_t off      = addr - base;
        int      reg;

        /* ATA registers take priority over ROM */
        if (size == 4) {
            reg = ata_ide_offset_to_reg(off);
            if (reg == 0) {
                /* movem.l / move.l: two consecutive word reads from DATA port */
                uint16_t hi = ata_ide_read16(&s->ide, 0);
                uint16_t lo = ata_ide_read16(&s->ide, 0);
                return ((uint32_t)hi << 16) | lo;
            }
        }
        if (size == 2) {
            reg = ata_ide_offset_to_reg(off);
            if (reg == 0)
                return ata_ide_read16(&s->ide, 0);
        }
        reg = ata_ide_offset_to_reg(off);
        if (reg >= 0)
            return ata_ide_read8(&s->ide, (uint8_t)reg);

        /* ROM: header (0-3), nibble-bootldr (4-0xFFF), device binary (0x2000+) */
        if (s->rom) {
            uint8_t b0 = ripple_rom_byte(s->rom, s->rom_size, off);
            if (size == 1) return b0;
            if (size == 2) {
                uint8_t b1 = ripple_rom_byte(s->rom, s->rom_size, off + 1u);
                return (uint32_t)(((uint16_t)b0 << 8) | b1);
            }
        }
    }

    return 0xFFFFFFFFu;
}

static void ripple_bus_write(BellatrixExpansion *exp,
                             uint32_t addr, uint32_t value, unsigned int size)
{
    (void)exp;

    if (bellatrix_zorro2_in_config_window(addr)) {
        switch (size) {
        case 1: bellatrix_zorro2_config_write8(addr,  (uint8_t)value);         break;
        case 2: bellatrix_zorro2_config_write16(addr, (uint16_t)value);        break;
        case 4: bellatrix_zorro2_config_write32(addr, (uint32_t)value);        break;
        default: break;
        }
        return;
    }

    if (bellatrix_zorro2_in_board_window(addr)) {
        LideCdromState *s = (LideCdromState *)exp->desc.userdata;
        uint32_t base     = bellatrix_zorro2_board_base("lide.cdrom");
        uint32_t off      = addr - base;
        int      reg;

        if (size == 4) {
            reg = ata_ide_offset_to_reg(off);
            if (reg == 0) {
                ata_ide_write16(&s->ide, 0, (uint16_t)(value >> 16));
                ata_ide_write16(&s->ide, 0, (uint16_t)(value & 0xFFFFu));
                return;
            }
        }
        if (size == 2) {
            reg = ata_ide_offset_to_reg(off);
            if (reg == 0) {
                ata_ide_write16(&s->ide, 0, (uint16_t)value);
                return;
            }
        }

        reg = ata_ide_offset_to_reg(off);
        if (reg >= 0)
            ata_ide_write8(&s->ide, (uint8_t)reg, (uint8_t)value);
    }
}

static const BellatrixExpansionBusOps g_ripple_bus_ops = {
    .owns_address = ripple_bus_owns,
    .read         = ripple_bus_read,
    .write        = ripple_bus_write,
};

/* -----------------------------------------------------------------------
 * Expansion lifecycle ops
 * --------------------------------------------------------------------- */

static int ripple_exp_attach(BellatrixExpansion *exp, BellatrixMachine *machine)
{
    (void)exp; (void)machine;
    return 0;
}

static void ripple_exp_reset(BellatrixExpansion *exp)
{
    LideCdromState *s = (LideCdromState *)exp->desc.userdata;
    if (!s) return;
    ata_ide_channel_reset(&s->ide);
    atapi_cdrom_reset(&s->atapi);
}

static void ripple_exp_shutdown(BellatrixExpansion *exp)
{
    (void)exp;
}

static void ripple_exp_destroy(BellatrixExpansion *exp)
{
    LideCdromState *s = (LideCdromState *)exp->desc.userdata;
    if (!s) return;
    ata_ide_channel_free(&s->ide);
    /* rom is embedded static data — no free */
    free(s);
    exp->desc.userdata = NULL;
    g_state = NULL;
}

static const BellatrixExpansionOps g_ripple_exp_ops = {
    .attach   = ripple_exp_attach,
    .reset    = ripple_exp_reset,
    .shutdown = ripple_exp_shutdown,
    .destroy  = ripple_exp_destroy,
};

/* -----------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------- */

int lide_cdrom_register(BellatrixMachine *machine)
{
    static const uint8_t ac_bytes[AUTOCONFIG_ROM_BYTES] = {
        AC_TYPE_Z2 | AC_TYPE_DIAGVALID | AC_SIZE_64KB,  /* er_Type    */
        RIPPLE_PROD,                                      /* er_Product */
        0x00u,                                            /* er_Flags   */
        0x00u,                                            /* reserved   */
        RIPPLE_MFR_HI,                                    /* MFR high   */
        RIPPLE_MFR_LO,                                    /* MFR low    */
        0x00u, 0x00u, 0x00u, 0x01u,                       /* serial     */
        0x00u,                                            /* InitDiagVec high */
        0x04u,                                            /* InitDiagVec low  */
        0x00u, 0x00u, 0x00u, 0x00u,                       /* reserved   */
    };

    LideCdromState     *s;
    BellatrixExpansionDesc desc;
    int                 rc;

    if (g_state) {
        kprintf("[LIDE-CDROM] already registered\n");
        return -1;
    }

    s = (LideCdromState *)calloc(1, sizeof(*s));
    if (!s) return -2;

    /* Point to embedded ROM — no file I/O */
    s->rom      = g_lide_rom_data;
    s->rom_size = g_lide_rom_size;

    /* Build autoconfig nibble array */
    autoconfig_build(s->config_data, ac_bytes);

    /* Init ATAPI and IDE layers */
    atapi_cdrom_init(&s->atapi, &g_iso_media_ops, &s->media);
    if (ata_ide_channel_alloc(&s->ide) != 0) {
        free(s);
        return -3;
    }
    ata_ide_channel_init(&s->ide);
    s->ide.atapi_ctx  = &s->atapi;
    s->ide.atapi_exec = atapi_cdrom_exec;

    /* Build Zorro 2 board descriptor */
    memset(&s->board_desc, 0, sizeof(s->board_desc));
    s->board_desc.id          = "lide.cdrom";
    s->board_desc.config_data = s->config_data;
    s->board_desc.config_size = sizeof(s->config_data);
    s->board_desc.window_size = BELLATRIX_ZORRO2_WIN_64KB;
    /* Do NOT set rom/rom_size — bellatrix_zorro2_board_read8 would bypass
     * ripple_read8. All reads must go through ripple_read8 so that
     * ripple_rom_byte can apply the A0-not-connected nibble remapping. */
    s->board_desc.rom         = NULL;
    s->board_desc.rom_size    = 0;
    s->board_desc.userdata    = s;
    s->board_desc.ops         = &g_ripple_ops;

    /* Register with Zorro 2 autoconfig state machine */
    rc = bellatrix_zorro2_register_board(&s->board_desc);
    if (rc != 0) {
        free(s);
        return rc;
    }

    /* Register as expansion (for bus dispatching) */
    memset(&desc, 0, sizeof(desc));
    desc.id         = "lide.cdrom";
    desc.name       = "OAHR RIPPLE CD-ROM";
    desc.kind       = BELLATRIX_EXPANSION_BOARD;
    desc.priority   = 90;
    desc.userdata   = s;
    desc.zorro2_board = &s->board_desc;
    desc.bus_ops    = &g_ripple_bus_ops;
    desc.ops        = &g_ripple_exp_ops;

    rc = bellatrix_expansion_register(machine, &desc);
    if (rc != 0) {
        bellatrix_zorro2_unregister_board("lide.cdrom");
        free(s);
        return rc;
    }

    g_state = s;

    kprintf("[LIDE-CDROM] registered OAHR RIPPLE mfr=0x%04x prod=%u  rom=%zu bytes (embedded)\n",
            (unsigned)((RIPPLE_MFR_HI << 8) | RIPPLE_MFR_LO),
            (unsigned)RIPPLE_PROD,
            s->rom_size);

    return 0;
}

int lide_cdrom_insert_iso(BellatrixMachine *machine,
                          const void *data, size_t size)
{
    LideCdromState *s;
    BellatrixExpansion *exp;

    (void)machine;
    exp = bellatrix_expansion_find(machine, "lide.cdrom");
    if (!exp) return -1;
    s = (LideCdromState *)exp->desc.userdata;
    if (!s) return -1;

    s->media.mem_data    = (const uint8_t *)data;
    s->media.mem_size    = size;
    s->media.fn          = NULL;
    s->media.fn_ctx      = NULL;
    s->media.sector_count = (uint32_t)(size / ATAPI_SECTOR_SIZE);
    s->media.present     = 1;

    atapi_cdrom_insert(&s->atapi);
    ata_ide_channel_reset(&s->ide);

    kprintf("[LIDE-CDROM] ISO inserted: %u sectors\n",
            (unsigned)s->media.sector_count);
    return 0;
}

int lide_cdrom_attach_iso_fn(BellatrixMachine *machine,
                             iso_read_fn fn, void *ctx,
                             uint32_t sector_count)
{
    LideCdromState *s;
    BellatrixExpansion *exp;

    (void)machine;
    exp = bellatrix_expansion_find(machine, "lide.cdrom");
    if (!exp) return -1;
    s = (LideCdromState *)exp->desc.userdata;
    if (!s) return -1;

    s->media.fn           = fn;
    s->media.fn_ctx       = ctx;
    s->media.mem_data     = NULL;
    s->media.mem_size     = 0;
    s->media.sector_count = sector_count;
    s->media.present      = 1;

    atapi_cdrom_insert(&s->atapi);
    ata_ide_channel_reset(&s->ide);

    kprintf("[LIDE-CDROM] callback ISO attached: %u sectors\n",
            (unsigned)sector_count);
    return 0;
}

void lide_cdrom_eject(BellatrixMachine *machine)
{
    LideCdromState *s;
    BellatrixExpansion *exp;

    exp = bellatrix_expansion_find(machine, "lide.cdrom");
    if (!exp) return;
    s = (LideCdromState *)exp->desc.userdata;
    if (!s) return;

    s->media.present     = 0;
    s->media.mem_data    = NULL;
    s->media.mem_size    = 0;
    s->media.fn          = NULL;
    s->media.fn_ctx      = NULL;
    s->media.sector_count = 0;

    atapi_cdrom_eject(&s->atapi);
    ata_ide_channel_reset(&s->ide);

    kprintf("[LIDE-CDROM] media ejected\n");
}
