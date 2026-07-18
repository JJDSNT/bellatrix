#include "machine/expansions/lide_cdrom/lide_cdrom.h"

#include "machine/expansions/lide_cdrom/ata_ide.h"
#include "machine/expansions/lide_cdrom/atapi_cdrom.h"
#include "machine/autoconfig/autoconfig.h"
#include "machine/bus/board_registry.h"
#include "machine/bus/zorro2/zorro2_bus.h"   /* BELLATRIX_ZORRO2_WIN_128KB */
#include "machine/expansion.h"
#include "machine/machine.h"

#include "support.h"

#include <stdlib.h>
#include <string.h>

/* Embedded ROMs — generated at build time by scripts/rom_to_c.py */
extern const unsigned char g_lide_rom_data[];
extern const size_t        g_lide_rom_size;

/* ODFileSystem handler binary for the second ROM bank (128 KB board).
 * Generated from external/ODFileSystem/build/amiga/ODFileSystem.
 * Served BYTEWIDE at board offsets 0x10000..0x1FFFF (even addresses only).
 * When present, the bootldr detects no 'LIDE' trailer in the second bank
 * and loads+relocates this binary as a CDFileSystem (DosType CD01). */
#if BELLATRIX_ENABLE_ODFS
extern const unsigned char g_odfs_data[];
extern const size_t        g_odfs_size;
#endif

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
} LideCdromState;

/* Global singleton (one CD-ROM expansion per machine for now) */
static LideCdromState *g_state;

/*
 * Autoconfig / base assignment goes through the self-registering board_registry
 * (the single Autoconfig authority), not the legacy zorro2 registry. lide is an
 * EXTERNAL board: map == NULL, so map() is never called; the walker only latches
 * the guest-assigned base into s_lide_board.map_base. The window is served per
 * access by the ripple_bus_* ops below, routed through the machine bus dispatch
 * (is_z2_board_addr -> bellatrix_boards_external_window_owner).
 *
 * The descriptor is always linked in; enabled flips to 1 in lide_cdrom_register
 * so lide is presented only when a CD/HDF is actually attached.
 */
static uint8_t s_lide_config[AUTOCONFIG_DATA_SIZE];

static struct ExpansionBoard s_lide_board = {
    .rom_file = s_lide_config,
    .rom_size = BELLATRIX_ZORRO2_WIN_128KB,
    .map_base = 0u,
    .is_z3    = 0u,
    .enabled  = 0u,
    .map      = NULL,
};
BELLATRIX_REGISTER_BOARD_Z2(s_lide_board);

static uint32_t lide_window_base(void) { return s_lide_board.map_base; }
static int      lide_configured(void)  { return s_lide_board.map_base != 0u; }

/* -----------------------------------------------------------------------
 * Expansion bus ops (dispatched by expansion.c)
 *
 * lide owns exactly its assigned Zorro II window [base, base+128KB). The
 * Autoconfig window is handled upstream by the board_registry walker, so it is
 * not claimed here. owns_address gates read/write, so both may assume the
 * address lies inside the window.
 * --------------------------------------------------------------------- */

static int ripple_bus_owns(BellatrixExpansion *exp, uint32_t addr)
{
    uint32_t base = lide_window_base();
    (void)exp;

    if (!lide_configured())
        return 0;
    return addr >= base && addr < base + BELLATRIX_ZORRO2_WIN_128KB;
}

static uint32_t ripple_bus_read(BellatrixExpansion *exp,
                                uint32_t addr, unsigned int size)
{
    (void)exp;

    {
        LideCdromState *s = (LideCdromState *)exp->desc.userdata;
        uint32_t base     = lide_window_base();
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

        /* Second bank (0x10000..0x1FFFF): ODFileSystem binary, BYTEWIDE encoded.
         * board[0x10000 + N*2] = odfs_byte[N].  Odd addresses return 0xFF.
         * The bootldr checks for 'LIDE' at 0x1FFF8-0x1FFFE; since our binary
         * ends well before that, 0xFF is returned there → CDFileSystem detected. */
#if BELLATRIX_ENABLE_ODFS
        if (off >= 0x10000u && off < 0x20000u) {
            uint32_t bank_off = off - 0x10000u;
            if (bank_off & 1u) return 0xFFu;       /* odd address: not mapped */
            uint32_t byte_idx = bank_off >> 1;
            /* Log first 4 and trailer reads so we can confirm _relocate is running */
            if (byte_idx < 4u || off >= 0x1FFF0u)
                kprintf("[LIDE-CDROM] bank2 off=0x%05x byte=%u/%u\n",
                        (unsigned)off, (unsigned)byte_idx, (unsigned)g_odfs_size);
            uint8_t  b = (byte_idx < g_odfs_size) ? g_odfs_data[byte_idx] : 0xFFu;
            if (size == 1) return b;
            uint8_t b1 = (byte_idx + 1u < g_odfs_size) ? g_odfs_data[byte_idx + 1u] : 0xFFu;
            return (size == 2) ? (((uint32_t)b << 8) | b1) : 0xFFu;
        }
#endif

        /* ROM: header (0-3), nibble-bootldr (4-0xFFF), device binary (0x2000+) */
        if (s->rom) {
            /* Log first read of device binary so we know bootldr is loading lide.device */
            if (off == RIPPLE_ROM_DEVICE_BOARD)
                kprintf("[LIDE-CDROM] device binary first read (bootldr loading lide.device)\n");
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

    {
        LideCdromState *s = (LideCdromState *)exp->desc.userdata;
        uint32_t base     = lide_window_base();
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
    /* Stop presenting the board in Autoconfig once torn down. */
    s_lide_board.enabled  = 0u;
    s_lide_board.map_base = 0u;
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
        AC_TYPE_Z2 | AC_TYPE_DIAGVALID | AC_SIZE_128KB, /* er_Type    */
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

    /* Build the nibble-encoded Autoconfig image into the static board buffer.
     * The board_registry walker answers the config window straight from
     * s_lide_board.rom_file (= s_lide_config). */
    autoconfig_build(s->config_data, ac_bytes);
    memcpy(s_lide_config, s->config_data, sizeof(s_lide_config));

    /* Init ATAPI and IDE layers */
    atapi_cdrom_init(&s->atapi, &g_iso_media_ops, &s->media);
    if (ata_ide_channel_alloc(&s->ide) != 0) {
        free(s);
        return -3;
    }
    ata_ide_channel_init(&s->ide);
    s->ide.atapi_ctx   = &s->atapi;
    s->ide.atapi_exec  = atapi_cdrom_exec;
    s->ide.atapi_reset = (void (*)(void *))atapi_cdrom_reset;

    /* Present the board through the self-registering board_registry: the
     * static descriptor is already linked in; enable it and hand it the
     * Autoconfig image. A guest base write latches s_lide_board.map_base.
     * 128 KB window: first 64 KB = lide.device ROM, second 64 KB = ODFileSystem.
     * All window reads go through ripple_bus_read so ripple_rom_byte can apply
     * the A0-not-connected nibble remapping (no plain DIRECT ROM here). */
    s_lide_board.map_base = 0u;
    s_lide_board.enabled  = 1u;

    /* Register as expansion (for per-access bus dispatch only). */
    memset(&desc, 0, sizeof(desc));
    desc.id         = "lide.cdrom";
    desc.name       = "OAHR RIPPLE CD-ROM";
    desc.kind       = BELLATRIX_EXPANSION_BOARD;
    desc.priority   = 90;
    desc.userdata   = s;
    desc.bus_ops    = &g_ripple_bus_ops;
    desc.ops        = &g_ripple_exp_ops;

    rc = bellatrix_expansion_register(machine, &desc);
    if (rc != 0) {
        s_lide_board.enabled = 0u;
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
    const uint8_t *bytes = (const uint8_t *)data;

    (void)machine;
    exp = bellatrix_expansion_find(machine, "lide.cdrom");
    if (!exp) {
        kprintf("[LIDE-CDROM] insert_iso: expansion not found\n");
        return -1;
    }
    s = (LideCdromState *)exp->desc.userdata;
    if (!s) {
        kprintf("[LIDE-CDROM] insert_iso: no state\n");
        return -1;
    }

    if (!data || size == 0) {
        kprintf("[LIDE-CDROM] insert_iso: NULL or empty data\n");
        return -1;
    }
    if (size % ATAPI_SECTOR_SIZE != 0) {
        kprintf("[LIDE-CDROM] insert_iso: size %u not a multiple of %u; truncating tail\n",
                (unsigned)size, (unsigned)ATAPI_SECTOR_SIZE);
        size -= size % ATAPI_SECTOR_SIZE;
    }
    if (size < 17u * ATAPI_SECTOR_SIZE) {
        kprintf("[LIDE-CDROM] insert_iso: too small for ISO9660 PVD (%u sectors)\n",
                (unsigned)(size / ATAPI_SECTOR_SIZE));
        return -1;
    }

    s->media.mem_data     = bytes;
    s->media.mem_size     = size;
    s->media.fn           = NULL;
    s->media.fn_ctx       = NULL;
    s->media.sector_count = (uint32_t)(size / ATAPI_SECTOR_SIZE);
    s->media.present      = 1;

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

int lide_hd_attach(BellatrixMachine *machine,
                   lide_hd_read_fn read_fn, lide_hd_write_fn write_fn,
                   void *ctx, uint32_t sector_count)
{
    LideCdromState *s;
    BellatrixExpansion *exp;

    exp = bellatrix_expansion_find(machine, "lide.cdrom");
    if (!exp) {
        kprintf("[LIDE-HD] attach: expansion not found\n");
        return -1;
    }
    s = (LideCdromState *)exp->desc.userdata;
    if (!s) return -1;

    if (!read_fn || sector_count == 0) {
        kprintf("[LIDE-HD] attach: invalid backend\n");
        return -1;
    }

    s->ide.disk_ctx     = ctx;
    s->ide.disk_read    = read_fn;
    s->ide.disk_write   = write_fn;
    s->ide.disk_sectors = sector_count;

    ata_ide_channel_reset(&s->ide);

    kprintf("[LIDE-HD] HD attached: %u sectors (%u MB)%s\n",
            (unsigned)sector_count, (unsigned)(sector_count / 2048u),
            write_fn ? "" : " read-only");
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
