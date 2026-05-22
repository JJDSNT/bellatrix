#include "machine/expansions/cdrom/media/cdrom_atapi.h"

#include <string.h>

static uint16_t bellatrix_atapi_be16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t bellatrix_atapi_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static void bellatrix_atapi_store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

void bellatrix_atapi_cdrom_set_sense(
    BellatrixAtapiCdrom *atapi,
    uint8_t key,
    uint8_t asc,
    uint8_t ascq
)
{
    if (!atapi) {
        return;
    }

    atapi->sense.key = key;
    atapi->sense.asc = asc;
    atapi->sense.ascq = ascq;
}

void bellatrix_atapi_cdrom_clear_sense(BellatrixAtapiCdrom *atapi)
{
    if (!atapi) {
        return;
    }

    atapi->sense.key = 0;
    atapi->sense.asc = 0;
    atapi->sense.ascq = 0;
}

void bellatrix_atapi_cdrom_init(
    BellatrixAtapiCdrom *atapi,
    BellatrixCdromDevice *device
)
{
    if (!atapi) {
        return;
    }

    memset(atapi, 0, sizeof(*atapi));
    atapi->device = device;
}

void bellatrix_atapi_cdrom_reset(BellatrixAtapiCdrom *atapi)
{
    if (!atapi) {
        return;
    }

    bellatrix_atapi_cdrom_clear_sense(atapi);
}

static bool bellatrix_atapi_cmd_test_unit_ready(
    BellatrixAtapiCdrom *atapi,
    BellatrixAtapiResult *result
)
{
    if (!bellatrix_cdrom_device_has_media(atapi->device)) {
        bellatrix_atapi_cdrom_set_sense(
            atapi,
            BELLATRIX_ATAPI_SENSE_NOT_READY,
            0x3A,
            0x00
        );
        result->check_condition = true;
        return false;
    }

    bellatrix_atapi_cdrom_clear_sense(atapi);
    result->transfer_length = 0;
    result->transfer_buffer = NULL;
    return true;
}

static bool bellatrix_atapi_cmd_inquiry(
    BellatrixAtapiCdrom *atapi,
    BellatrixAtapiResult *result
)
{
    uint8_t *buf = atapi->data_buffer;

    memset(buf, 0, 36);
    buf[0] = 0x05;
    buf[1] = 0x80;
    buf[3] = 0x21;
    buf[4] = 31;

    memcpy(&buf[8], "BELLATRIX", 9);
    memcpy(&buf[16], "PLUGIN CD-ROM", 13);
    memcpy(&buf[32], "1.0 ", 4);

    result->transfer_buffer = buf;
    result->transfer_length = 36;

    bellatrix_atapi_cdrom_clear_sense(atapi);
    return true;
}

static bool bellatrix_atapi_cmd_request_sense(
    BellatrixAtapiCdrom *atapi,
    BellatrixAtapiResult *result
)
{
    uint8_t *buf = atapi->data_buffer;

    memset(buf, 0, 18);
    buf[0] = 0x70;
    buf[2] = atapi->sense.key;
    buf[7] = 10;
    buf[12] = atapi->sense.asc;
    buf[13] = atapi->sense.ascq;

    result->transfer_buffer = buf;
    result->transfer_length = 18;
    return true;
}

static bool bellatrix_atapi_cmd_read_capacity(
    BellatrixAtapiCdrom *atapi,
    BellatrixAtapiResult *result
)
{
    uint8_t *buf = atapi->data_buffer;
    uint32_t sectors;

    if (!bellatrix_cdrom_device_has_media(atapi->device)) {
        bellatrix_atapi_cdrom_set_sense(
            atapi,
            BELLATRIX_ATAPI_SENSE_NOT_READY,
            0x3A,
            0x00
        );
        result->check_condition = true;
        return false;
    }

    sectors = bellatrix_cdrom_device_sector_count(atapi->device);

    memset(buf, 0, 8);
    if (sectors > 0) {
        bellatrix_atapi_store_be32(&buf[0], sectors - 1);
    }
    bellatrix_atapi_store_be32(&buf[4], BELLATRIX_CDROM_SECTOR_SIZE);

    result->transfer_buffer = buf;
    result->transfer_length = 8;

    bellatrix_atapi_cdrom_clear_sense(atapi);
    return true;
}

static bool bellatrix_atapi_cmd_read10(
    BellatrixAtapiCdrom *atapi,
    const uint8_t *cdb,
    BellatrixAtapiResult *result
)
{
    uint32_t lba;
    uint16_t count;
    BellatrixCdromStatus st;

    lba = bellatrix_atapi_be32(&cdb[2]);
    count = bellatrix_atapi_be16(&cdb[7]);

    if (count == 0) {
        count = 65535u;
    }

    st = bellatrix_cdrom_device_read(
        atapi->device,
        lba,
        count,
        atapi->data_buffer,
        sizeof(atapi->data_buffer)
    );

    if (st != BELLATRIX_CDROM_STATUS_OK) {
        bellatrix_atapi_cdrom_set_sense(
            atapi,
            BELLATRIX_ATAPI_SENSE_ILLEGAL_REQUEST,
            0x21,
            0x00
        );
        result->check_condition = true;
        return false;
    }

    result->transfer_buffer = atapi->data_buffer;
    result->transfer_length = (uint32_t)count * BELLATRIX_CDROM_SECTOR_SIZE;

    bellatrix_atapi_cdrom_clear_sense(atapi);
    return true;
}

static bool bellatrix_atapi_cmd_mode_sense_6(
    BellatrixAtapiCdrom *atapi,
    BellatrixAtapiResult *result
)
{
    uint8_t *buf = atapi->data_buffer;

    memset(buf, 0, 8);
    buf[0] = 6;

    result->transfer_buffer = buf;
    result->transfer_length = 8;

    bellatrix_atapi_cdrom_clear_sense(atapi);
    return true;
}

bool bellatrix_atapi_cdrom_execute_packet(
    BellatrixAtapiCdrom *atapi,
    const uint8_t *cdb,
    size_t cdb_len,
    BellatrixAtapiResult *result
)
{
    uint8_t opcode;

    if (!atapi || !cdb || !result) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->irq = true;

    if (cdb_len < 1) {
        return false;
    }

    opcode = cdb[0];

    switch (opcode) {
    case BELLATRIX_ATAPI_CMD_TEST_UNIT_READY:
        return bellatrix_atapi_cmd_test_unit_ready(atapi, result);
    case BELLATRIX_ATAPI_CMD_INQUIRY:
        return bellatrix_atapi_cmd_inquiry(atapi, result);
    case BELLATRIX_ATAPI_CMD_REQUEST_SENSE:
        return bellatrix_atapi_cmd_request_sense(atapi, result);
    case BELLATRIX_ATAPI_CMD_READ_CAPACITY:
        return bellatrix_atapi_cmd_read_capacity(atapi, result);
    case BELLATRIX_ATAPI_CMD_READ_10:
        return bellatrix_atapi_cmd_read10(atapi, cdb, result);
    case BELLATRIX_ATAPI_CMD_MODE_SENSE_6:
        return bellatrix_atapi_cmd_mode_sense_6(atapi, result);
    default:
        bellatrix_atapi_cdrom_set_sense(
            atapi,
            BELLATRIX_ATAPI_SENSE_ILLEGAL_REQUEST,
            0x20,
            0x00
        );
        result->check_condition = true;
        return false;
    }
}
