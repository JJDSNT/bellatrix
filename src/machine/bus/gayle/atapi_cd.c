// src/machine/bus/gayle/atapi_cd.c

/*
 * ============================================================================
 * Legacy ATAPI CD-ROM implementation
 * ============================================================================
 *
 * This file originally mixed three different responsibilities:
 *
 *   1. ATAPI/SCSI CD-ROM command handling
 *   2. IDE/ATA transport state machine
 *   3. ISO image/media access
 *
 * As Bellatrix evolves toward a more modular architecture, this file should
 * be progressively decomposed into independent layers.
 *
 * ---------------------------------------------------------------------------
 * New architecture
 * ---------------------------------------------------------------------------
 *
 * storage/iso
 *     Raw ISO image/media backend
 *
 * devices/cdrom
 *     Generic CD-ROM device behavior
 *     - media insertion/ejection
 *     - sector reads
 *     - TOC
 *     - sense handling
 *     - SCSI/ATAPI command implementation
 *
 * machine/bus/gayle
 *     IDE/ATA/ATAPI transport only
 *     - PACKET command handling
 *     - DRQ/BSY/ERR state
 *     - IRQ signaling
 *     - PIO transfers
 *     - ATA registers
 *
 * boards/
 *     Optional Zorro/Autoconfig controllers and ROMs
 *
 * ---------------------------------------------------------------------------
 * Migration plan
 * ---------------------------------------------------------------------------
 *
 * The following logic should move to:
 *
 *   devices/cdrom/cdrom_atapi.c
 *       - cmd_inquiry()
 *       - cmd_test_unit_ready()
 *       - cmd_request_sense()
 *       - cmd_read_capacity()
 *       - cmd_read10()
 *       - cmd_read_toc()
 *       - cmd_prevent_allow()
 *       - sense handling helpers
 *       - TOC/MSF helpers
 *
 *   devices/cdrom/cdrom_device.c
 *       - media presence/change tracking
 *       - generic sector access
 *
 *   machine/bus/gayle/gayle_ide.c
 *       - packet phases
 *       - word transfers
 *       - DRQ/BSY/ERR handling
 *       - ATA status/error registers
 *       - packet receive state machine
 *
 * ---------------------------------------------------------------------------
 * Important
 * ---------------------------------------------------------------------------
 *
 * The CD-ROM device layer must NOT depend directly on:
 *
 *   - Gayle
 *   - ATA registers
 *   - IDE transport details
 *   - ARM-side launcher logic
 *
 * Likewise, the ISO backend should remain completely unaware of:
 *
 *   - ATAPI
 *   - SCSI
 *   - Gayle
 *   - Zorro
 *
 * ---------------------------------------------------------------------------
 * Compatibility hacks
 * ---------------------------------------------------------------------------
 *
 * Any AROS-specific or bootability hacks (such as patching the ISO PVD
 * System Identifier to "AMIGA BOOT") should eventually become optional
 * compatibility layers instead of living inside the generic CD-ROM device.
 *
 * ============================================================================
 */

#include "machine/bus/gayle/atapi_cd.h"
#include "support.h"

#include <string.h>

/* ----------------------------- helpers ----------------------------- */

static void set_sense(AtapiCd *cd, uint8_t key, uint8_t asc, uint8_t ascq)
{
    cd->sense_key = key;
    cd->asc = asc;
    cd->ascq = ascq;
}

static void prepare_data(AtapiCd *cd, size_t size)
{
    cd->data_len = size;
    cd->data_pos = 0;
    cd->phase = ATAPI_PHASE_DATA_IN;
}

static uint32_t be32(const uint8_t *p)
{
    return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
}

static void lba_to_msf(uint32_t lba, uint8_t *m, uint8_t *s, uint8_t *f)
{
    lba += 150; /* 2-second pregap */
    *f = lba % 75;
    lba /= 75;
    *s = lba % 60;
    *m = lba / 60;
}

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xff;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

/* ----------------------------- SCSI commands ----------------------------- */

static void cmd_inquiry(AtapiCd *cd)
{
    size_t alen = cd->packet[4];

    memset(cd->data, 0, 36);

    cd->data[0] = 0x05; /* CD-ROM */
    cd->data[1] = 0x80; /* removable */
    cd->data[2] = 0x00;
    cd->data[3] = 0x21;
    cd->data[4] = 31;

    memcpy(&cd->data[8],  "BELLATRIX", 9);
    memcpy(&cd->data[16], "VIRTUAL CD-ROM  ", 16);

    prepare_data(cd, alen < 36u ? alen : 36u);
}

static void cmd_test_unit_ready(AtapiCd *cd)
{
    if (iso_image_present(cd->iso)) {
        set_sense(cd, 0x00, 0x00, 0x00); /* clear any stale sense */
    } else {
        set_sense(cd, 0x02, 0x3A, 0x00); /* NOT READY / NO MEDIA */
    }
    cd->phase = ATAPI_PHASE_STATUS;
}

static void cmd_request_sense(AtapiCd *cd)
{
    size_t alen = cd->packet[4];

    memset(cd->data, 0, 18);

    cd->data[0] = 0x70;
    cd->data[2] = cd->sense_key;
    cd->data[7] = 10; /* additional sense length */
    cd->data[12] = cd->asc;
    cd->data[13] = cd->ascq;

    set_sense(cd, 0x00, 0x00, 0x00); /* reading clears sense */

    prepare_data(cd, alen < 18u ? alen : 18u);
}

static void cmd_read_capacity(AtapiCd *cd)
{
    memset(cd->data, 0, 8);

    uint32_t last_lba = iso_image_sector_count(cd->iso) - 1;

    write_be32(&cd->data[0], last_lba);
    write_be32(&cd->data[4], ISO_SECTOR_SIZE);

    prepare_data(cd, 8);
}

static void cmd_read10(AtapiCd *cd)
{
    uint32_t lba = be32(&cd->packet[2]);
    uint16_t count = (cd->packet[7] << 8) | cd->packet[8];

    size_t total = (size_t)count * ISO_SECTOR_SIZE;

    if (total > cd->data_capacity)
    {
        set_sense(cd, 0x05, 0x21, 0x00);
        cd->phase = ATAPI_PHASE_STATUS;
        return;
    }

    if (!iso_image_read_sectors(cd->iso, lba, count, cd->data))
    {
        set_sense(cd, 0x05, 0x21, 0x00);
        cd->phase = ATAPI_PHASE_STATUS;
        return;
    }

    /* Patch PVD System Identifier to "AMIGA BOOT" so any ISO looks bootable
     * to AROS/mounter code that checks the System ID field (ISO 9660 §8.4.5).
     * The PVD is the first sector of the volume descriptor set at LBA 16.
     * System ID occupies bytes 8-39 (32 bytes, space-padded). */
    if (lba == 16 && count >= 1 && total >= 40)
    {
        static const char amiga_boot_id[32] =
            "AMIGA BOOT                      ";
        memcpy(&cd->data[8], amiga_boot_id, 32);
        kprintf("[ATAPI] PVD System ID patched -> AMIGA BOOT\n");
    }

    prepare_data(cd, total);
}

static void cmd_read_toc(AtapiCd *cd)
{
    bool msf      = (cd->packet[1] & 0x02) != 0;
    uint8_t fmt   = cd->packet[2] & 0x0f;
    uint16_t alen = ((uint16_t)cd->packet[7] << 8) | cd->packet[8];

    /* Only support format 0 (standard TOC) */
    if (fmt != 0) {
        set_sense(cd, 0x05, 0x20, 0x00); /* ILLEGAL REQUEST */
        cd->phase = ATAPI_PHASE_STATUS;
        return;
    }

    uint32_t last_lba = iso_image_sector_count(cd->iso); /* = lead-out LBA */

    memset(cd->data, 0, 20);

    /* Header: TOC data length (18 = 20 - 2), first/last track */
    cd->data[0] = 0x00; cd->data[1] = 0x12;
    cd->data[2] = 0x01; /* first track */
    cd->data[3] = 0x01; /* last track */

    /* Track 1 descriptor */
    cd->data[4] = 0x00;
    cd->data[5] = 0x14; /* ADR=1 (Q-channel mode 1), control=4 (data) */
    cd->data[6] = 0x01;
    cd->data[7] = 0x00;
    if (msf) {
        uint8_t m, s, f;
        lba_to_msf(0, &m, &s, &f);
        cd->data[8] = 0; cd->data[9] = m; cd->data[10] = s; cd->data[11] = f;
    } else {
        write_be32(&cd->data[8], 0);
    }

    /* Lead-out descriptor (track 0xAA) */
    cd->data[12] = 0x00;
    cd->data[13] = 0x14;
    cd->data[14] = 0xAA;
    cd->data[15] = 0x00;
    if (msf) {
        uint8_t m, s, f;
        lba_to_msf(last_lba, &m, &s, &f);
        cd->data[16] = 0; cd->data[17] = m; cd->data[18] = s; cd->data[19] = f;
    } else {
        write_be32(&cd->data[16], last_lba);
    }

    size_t len = (alen < 20u) ? alen : 20u;
    prepare_data(cd, len);
}

static void cmd_prevent_allow(AtapiCd *cd)
{
    /* Virtual drive: medium removal is always allowed — silently ignore. */
    set_sense(cd, 0x00, 0x00, 0x00);
    cd->phase = ATAPI_PHASE_STATUS;
}

/* ----------------------------- dispatcher ----------------------------- */

static void handle_packet(AtapiCd *cd)
{
    uint8_t op = cd->packet[0];

    kprintf("[ATAPI] cmd %02x (cdb: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x)\n",
            (unsigned)op,
            (unsigned)cd->packet[0], (unsigned)cd->packet[1],
            (unsigned)cd->packet[2], (unsigned)cd->packet[3],
            (unsigned)cd->packet[4], (unsigned)cd->packet[5],
            (unsigned)cd->packet[6], (unsigned)cd->packet[7],
            (unsigned)cd->packet[8], (unsigned)cd->packet[9],
            (unsigned)cd->packet[10], (unsigned)cd->packet[11]);

    switch (op)
    {
        case 0x00: cmd_test_unit_ready(cd); break;
        case 0x03: cmd_request_sense(cd); break;
        case 0x12: cmd_inquiry(cd); break;
        case 0x1e: cmd_prevent_allow(cd); break;
        case 0x25: cmd_read_capacity(cd); break;
        case 0x28: cmd_read10(cd); break;
        case 0x43: cmd_read_toc(cd); break;

        default:
            kprintf("[ATAPI] unhandled cmd %02x\n", (unsigned)op);
            set_sense(cd, 0x05, 0x20, 0x00); /* ILLEGAL COMMAND */
            cd->phase = ATAPI_PHASE_STATUS;
            break;
    }
}

/* ----------------------------- public API ----------------------------- */

void atapi_cd_init(AtapiCd *cd, IsoImage *iso, uint8_t *buffer, size_t buffer_size)
{
    cd->iso = iso;
    cd->data = buffer;
    cd->data_capacity = buffer_size;
    atapi_cd_reset(cd);
}

void atapi_cd_reset(AtapiCd *cd)
{
    cd->phase = ATAPI_PHASE_IDLE;
    cd->packet_pos = 0;
    cd->data_len = 0;
    cd->data_pos = 0;
    set_sense(cd, 0, 0, 0);
    cd->media_changed = true;
}

bool atapi_cd_media_present(const AtapiCd *cd)
{
    return iso_image_present(cd->iso);
}

bool atapi_cd_media_changed(const AtapiCd *cd)
{
    return cd->media_changed;
}

void atapi_cd_clear_media_changed(AtapiCd *cd)
{
    cd->media_changed = false;
}

void atapi_cd_begin_packet(AtapiCd *cd)
{
    cd->phase = ATAPI_PHASE_PACKET_IN;
    cd->packet_pos = 0;
}

bool atapi_cd_write_packet_word(AtapiCd *cd, uint16_t word)
{
    if (cd->phase != ATAPI_PHASE_PACKET_IN)
        return false;

    cd->packet[cd->packet_pos++] = (word >> 8) & 0xff;
    cd->packet[cd->packet_pos++] = word & 0xff;

    if (cd->packet_pos >= ATAPI_PACKET_SIZE)
    {
        handle_packet(cd);
    }

    return true;
}

bool atapi_cd_has_data(const AtapiCd *cd)
{
    return cd->phase == ATAPI_PHASE_DATA_IN && cd->data_pos < cd->data_len;
}

uint16_t atapi_cd_read_data_word(AtapiCd *cd)
{
    if (!atapi_cd_has_data(cd))
        return 0;

    uint16_t v = (cd->data[cd->data_pos] << 8) |
                 (cd->data[cd->data_pos + 1]);

    cd->data_pos += 2;

    if (cd->data_pos >= cd->data_len)
        cd->phase = ATAPI_PHASE_STATUS;

    return v;
}

size_t atapi_cd_remaining_data(const AtapiCd *cd)
{
    return cd->data_len - cd->data_pos;
}

uint8_t atapi_cd_status(const AtapiCd *cd)
{
    if (cd->phase == ATAPI_PHASE_DATA_IN)
        return 0x08; // DRQ

    return 0x40; // DRDY
}

uint8_t atapi_cd_error(const AtapiCd *cd)
{
    return cd->sense_key ? 0x01 : 0x00;
}
