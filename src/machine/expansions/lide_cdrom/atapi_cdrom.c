#include "machine/expansions/lide_cdrom/atapi_cdrom.h"

#include "support.h"
#include "machine/machine.h"

#include <string.h>

#include <stdlib.h>

/* Per-command trace logging: enable with HARNESS_CD_TRACE=1.
 * Off by default — during sustained CD I/O this logging is heavy enough
 * to throttle the emulator when stdout is a terminal. */
static int atapi_cd_trace_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("HARNESS_CD_TRACE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    return enabled;
}
#define CD_TRACE(...) do { if (atapi_cd_trace_enabled()) kprintf(__VA_ARGS__); } while (0)


/* ISO 9660 PVD patching */
#define ISO9660_PVD_LBA     16u   /* Primary Volume Descriptor is always at LBA 16 */
#define ISO9660_SYSID_OFF    8u   /* System Identifier offset within the sector      */
#define ISO9660_SYSID_LEN   32u   /* System Identifier field length (space-padded)   */

/* SCSI/ATAPI command codes */
#define CMD_TEST_UNIT_READY  0x00
#define CMD_REQUEST_SENSE    0x03
#define CMD_INQUIRY          0x12
#define CMD_START_STOP       0x1B
#define CMD_READ_CAPACITY    0x25
#define CMD_READ_10          0x28
#define CMD_READ_TOC         0x43
#define CMD_MODE_SENSE_10    0x5A

/* Sense keys */
#define SK_NO_SENSE          0x00
#define SK_NOT_READY         0x02
#define SK_ILLEGAL_REQUEST   0x05
#define SK_UNIT_ATTENTION    0x06

/* ASC codes */
#define ASC_MEDIUM_NOT_PRESENT   0x3A
#define ASC_MEDIUM_CHANGED       0x28
#define ASC_INVALID_COMMAND      0x20
#define ASC_INVALID_FIELD_IN_CDB 0x24
#define ASC_LBA_OUT_OF_RANGE     0x21

/*
 * patch_pvd_sysid — called after reading LBA 16.
 *
 * For a CD to boot on AROS/AmigaOS, the System Identifier field in the
 * Primary Volume Descriptor (bytes 8-39) must be "CDTV" (for CDTV/CD32)
 * or "AMIGA BOOT".  Plain PC/data ISOs have an empty or OS-specific
 * identifier, so AROS won't boot them.  Patch on the fly so any ISO 9660
 * disc is treated as bootable.
 */
static void patch_pvd_sysid(uint8_t *sector)
{
    /* Opt-in (HARNESS_CD_BOOTABLE=1): forging "AMIGA BOOT" makes the lide
     * mounter register the CD with bootPri=2, so it competes with the
     * floppy in the boot node list.  For the normal workflow — boot from
     * ADF, CD mounted as a data volume — the ISO must stay non-bootable. */
    static int enabled = -1;
    if (enabled < 0) {
        const char *env = getenv("HARNESS_CD_BOOTABLE");
        enabled = (env && env[0] != '\0' && env[0] != '0') ? 1 : 0;
    }
    if (!enabled)
        return;

    /* Verify PVD: type byte = 0x01, standard identifier = "CD001" */
    if (sector[0] != 0x01)
        return;
    if (memcmp(sector + 1, "CD001", 5) != 0)
        return;

    /* Already has an Amiga boot marker — nothing to do */
    if (memcmp(sector + ISO9660_SYSID_OFF, "CDTV",       4)  == 0 ||
        memcmp(sector + ISO9660_SYSID_OFF, "AMIGA BOOT", 10) == 0)
        return;

    memset(sector + ISO9660_SYSID_OFF, ' ', ISO9660_SYSID_LEN);
    memcpy(sector + ISO9660_SYSID_OFF, "AMIGA BOOT", 10);
    kprintf("[ATAPI] PVD sysid patched to 'AMIGA BOOT'\n");
}

static void set_sense(AtapiCdromState *s, uint8_t key, uint8_t asc, uint8_t ascq)
{
    s->sense_key = key;
    s->asc       = asc;
    s->ascq      = ascq;
}

void atapi_cdrom_init(AtapiCdromState *s,
                      const AtapiMediaOps *ops, void *ops_ctx)
{
    memset(s, 0, sizeof(*s));
    s->ops     = ops;
    s->ops_ctx = ops_ctx;
}

void atapi_cdrom_reset(AtapiCdromState *s)
{
    bool had_media = s->ops->present(s->ops_ctx);
    memset(&s->sense_key, 0, sizeof(s->sense_key));
    s->asc  = 0;
    s->ascq = 0;
    /* Per ATAPI spec: power-on/reset raises UNIT_ATTENTION if media present */
    s->unit_attention = had_media;
}

void atapi_cdrom_insert(AtapiCdromState *s)
{
    s->unit_attention = true;  /* media change — host must re-scan */
}

void atapi_cdrom_eject(AtapiCdromState *s)
{
    s->unit_attention = false;
}

/* ------------------------------------------------------------------ */

static int cmd_test_unit_ready(AtapiCdromState *s,
                                uint8_t *out, size_t max, size_t *len)
{
    (void)out; (void)max;
    *len = 0;
    if (!s->ops->present(s->ops_ctx)) {
        set_sense(s, SK_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0x00);
        return -1;
    }
    if (s->unit_attention) {
        s->unit_attention = false;
        set_sense(s, SK_UNIT_ATTENTION, ASC_MEDIUM_CHANGED, 0x00);
        kprintf("[ATAPI] UNIT_ATTENTION raised (medium changed)\n");
        return -1;
    }
    set_sense(s, SK_NO_SENSE, 0, 0);
    return 0;
}

static int cmd_request_sense(AtapiCdromState *s,
                              uint8_t *out, size_t max, size_t *len)
{
    if (max < 18) return -1;
    memset(out, 0, 18);
    out[0] = 0x70;                 /* current error, fixed format */
    out[2] = s->sense_key & 0x0Fu;
    out[7] = 10;                   /* additional sense length      */
    out[12] = s->asc;
    out[13] = s->ascq;
    *len = 18;
    set_sense(s, SK_NO_SENSE, 0, 0);
    return 0;
}

static int cmd_inquiry(AtapiCdromState *s,
                       uint8_t *out, size_t max, size_t *len)
{
    (void)s;
    if (max < 36) return -1;
    memset(out, 0, 36);
    out[0] = 0x05;        /* peripheral device type = CD-ROM        */
    out[1] = 0x80;        /* removable medium                       */
    out[2] = 0x02;        /* ANSI version = SCSI-2                  */
    out[3] = 0x32;        /* ATAPI / response data format           */
    out[4] = 31;          /* additional length                      */
    /* vendor, product, revision (padded with spaces) */
    memcpy(out + 8,  "BELLA   ", 8);
    memcpy(out + 16, "VIRTUAL CDROM   ", 16);
    memcpy(out + 32, "1.00", 4);
    *len = 36;
    return 0;
}

static int cmd_read_capacity(AtapiCdromState *s,
                              uint8_t *out, size_t max, size_t *len)
{
    uint32_t sectors;

    if (max < 8) return -1;
    if (!s->ops->present(s->ops_ctx)) {
        set_sense(s, SK_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        return -1;
    }

    sectors = s->ops->sector_count(s->ops_ctx);
    if (sectors == 0) sectors = 1;

    /* last LBA (big-endian) */
    uint32_t last = sectors - 1;
    out[0] = (uint8_t)(last >> 24);
    out[1] = (uint8_t)(last >> 16);
    out[2] = (uint8_t)(last >>  8);
    out[3] = (uint8_t)(last);
    /* block size = 2048 */
    out[4] = 0x00;
    out[5] = 0x00;
    out[6] = 0x08;
    out[7] = 0x00;
    *len = 8;
    return 0;
}

static int cmd_read_toc(AtapiCdromState *s, const uint8_t *pkt,
                        uint8_t *out, size_t max, size_t *len)
{
    uint32_t sectors;
    uint16_t toc_len;

    if (max < 20) return -1;
    if (!s->ops->present(s->ops_ctx)) {
        set_sense(s, SK_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        return -1;
    }

    sectors = s->ops->sector_count(s->ops_ctx);

    /* Minimal TOC: data track 1 from LBA 0, lead-out from sectors */
    memset(out, 0, 20);
    toc_len = 18;   /* 2 bytes header + 8 bytes track 1 + 8 bytes lead-out */
    out[0]  = (uint8_t)(toc_len >> 8);
    out[1]  = (uint8_t)(toc_len);
    out[2]  = 1;    /* first track */
    out[3]  = 1;    /* last track  */

    /* Track 1 descriptor */
    out[4]  = 0;
    out[5]  = 0x14; /* ADR/CTRL: data track */
    out[6]  = 1;    /* track number */
    out[7]  = 0;
    /* LBA 0 (big-endian) */
    out[8]  = 0; out[9] = 0; out[10] = 0; out[11] = 0;

    /* Lead-out descriptor */
    out[12] = 0;
    out[13] = 0x14;
    out[14] = 0xAA; /* lead-out track */
    out[15] = 0;
    out[16] = (uint8_t)(sectors >> 24);
    out[17] = (uint8_t)(sectors >> 16);
    out[18] = (uint8_t)(sectors >>  8);
    out[19] = (uint8_t)(sectors);

    *len = 20;
    (void)pkt;
    return 0;
}

static int cmd_mode_sense_10(AtapiCdromState *s,
                              uint8_t *out, size_t max, size_t *len)
{
    (void)s;
    if (max < 8) return -1;
    memset(out, 0, 8);
    out[0] = 0;
    out[1] = 6;   /* mode data length */
    *len = 8;
    return 0;
}

static int cmd_read_10(AtapiCdromState *s, const uint8_t *pkt,
                       uint8_t *out, size_t max, size_t *len)
{
    uint32_t lba;
    uint16_t count;
    size_t   needed;

    if (!s->ops->present(s->ops_ctx)) {
        set_sense(s, SK_NOT_READY, ASC_MEDIUM_NOT_PRESENT, 0);
        return -1;
    }

    lba   = ((uint32_t)pkt[2] << 24) | ((uint32_t)pkt[3] << 16) |
            ((uint32_t)pkt[4] <<  8) |  (uint32_t)pkt[5];
    count = (uint16_t)(((uint16_t)pkt[7] << 8) | pkt[8]);

    CD_TRACE("[ATAPI] READ_10 lba=%u count=%u max=%u frame=%u\n",
            (unsigned)lba, (unsigned)count, (unsigned)max,
            (unsigned)bellatrix_machine_get()->frame_counter);

    if (count == 0) {
        *len = 0;
        return 0;
    }

    needed = (size_t)count * ATAPI_SECTOR_SIZE;
    if (needed > max) {
        kprintf("[ATAPI] READ_10 OVERFLOW needed=%u max=%u\n",
                (unsigned)needed, (unsigned)max);
        set_sense(s, SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD_IN_CDB, 0);
        return -1;
    }

    if (s->ops->read_sectors(s->ops_ctx, lba, count, out) != 0) {
        kprintf("[ATAPI] READ_10 read_sectors FAILED lba=%u count=%u\n",
                (unsigned)lba, (unsigned)count);
        set_sense(s, SK_ILLEGAL_REQUEST, ASC_LBA_OUT_OF_RANGE, 0);
        return -1;
    }

    /* Patch the PVD System Identifier if it lacks an Amiga boot marker */
    if (lba <= ISO9660_PVD_LBA && lba + count > ISO9660_PVD_LBA) {
        uint32_t pvd_off = ISO9660_PVD_LBA - lba;
        patch_pvd_sysid(out + pvd_off * ATAPI_SECTOR_SIZE);
    }

    *len = needed;
    set_sense(s, SK_NO_SENSE, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ */

int atapi_cdrom_exec(void *ctx,
                     const uint8_t *pkt, size_t pkt_len,
                     uint8_t *buf_out, size_t buf_max,
                     size_t *data_len)
{
    AtapiCdromState *s = (AtapiCdromState *)ctx;

    (void)pkt_len;
    *data_len = 0;

    CD_TRACE("[ATAPI] cmd %02x media=%d\n", (unsigned)pkt[0], s->ops->present(s->ops_ctx));
    switch (pkt[0]) {
    case CMD_TEST_UNIT_READY:
        return cmd_test_unit_ready(s, buf_out, buf_max, data_len);
    case CMD_REQUEST_SENSE:
        return cmd_request_sense(s, buf_out, buf_max, data_len);
    case CMD_INQUIRY:
        return cmd_inquiry(s, buf_out, buf_max, data_len);
    case CMD_READ_CAPACITY:
        return cmd_read_capacity(s, buf_out, buf_max, data_len);
    case CMD_READ_TOC:
        return cmd_read_toc(s, pkt, buf_out, buf_max, data_len);
    case CMD_MODE_SENSE_10:
        return cmd_mode_sense_10(s, buf_out, buf_max, data_len);
    case CMD_READ_10:
        return cmd_read_10(s, pkt, buf_out, buf_max, data_len);
    case CMD_START_STOP:
        *data_len = 0;
        return 0;
    default:
        kprintf("[ATAPI] unhandled cmd %02x\n", (unsigned)pkt[0]);
        set_sense(s, SK_ILLEGAL_REQUEST, ASC_INVALID_COMMAND, 0);
        return -1;
    }
}
