// src/bus/gayle/atapi_cd.c

#include "bus/gayle/atapi_cd.h"

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
    memset(cd->data, 0, 36);

    cd->data[0] = 0x05; // CD-ROM
    cd->data[1] = 0x80; // removable
    cd->data[2] = 0x00;
    cd->data[3] = 0x21;
    cd->data[4] = 31;

    memcpy(&cd->data[8],  "BELLATRIX", 9);
    memcpy(&cd->data[16], "VIRTUAL CD-ROM", 15);

    prepare_data(cd, 36);
}

static void cmd_test_unit_ready(AtapiCd *cd)
{
    if (!iso_image_present(cd->iso))
    {
        set_sense(cd, 0x02, 0x3A, 0x00); // NOT READY / NO MEDIA
    }
    cd->phase = ATAPI_PHASE_STATUS;
}

static void cmd_request_sense(AtapiCd *cd)
{
    memset(cd->data, 0, 18);

    cd->data[0] = 0x70;
    cd->data[2] = cd->sense_key;
    cd->data[7] = 10;
    cd->data[12] = cd->asc;
    cd->data[13] = cd->ascq;

    prepare_data(cd, 18);
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
        set_sense(cd, 0x05, 0x21, 0x00); // ILLEGAL REQUEST
        cd->phase = ATAPI_PHASE_STATUS;
        return;
    }

    if (!iso_image_read_sectors(cd->iso, lba, count, cd->data))
    {
        set_sense(cd, 0x05, 0x21, 0x00);
        cd->phase = ATAPI_PHASE_STATUS;
        return;
    }

    prepare_data(cd, total);
}

/* ----------------------------- dispatcher ----------------------------- */

static void handle_packet(AtapiCd *cd)
{
    uint8_t op = cd->packet[0];

    switch (op)
    {
        case 0x12: cmd_inquiry(cd); break;
        case 0x00: cmd_test_unit_ready(cd); break;
        case 0x03: cmd_request_sense(cd); break;
        case 0x25: cmd_read_capacity(cd); break;
        case 0x28: cmd_read10(cd); break;

        default:
            set_sense(cd, 0x05, 0x20, 0x00); // illegal command
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