#ifndef BELLATRIX_EXPANSIONS_CDROM_MEDIA_CDROM_ATAPI_H
#define BELLATRIX_EXPANSIONS_CDROM_MEDIA_CDROM_ATAPI_H

#include "machine/expansions/cdrom/media/cdrom_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BELLATRIX_ATAPI_PACKET_SIZE 12

#define BELLATRIX_ATAPI_CMD_TEST_UNIT_READY   0x00
#define BELLATRIX_ATAPI_CMD_REQUEST_SENSE     0x03
#define BELLATRIX_ATAPI_CMD_INQUIRY           0x12
#define BELLATRIX_ATAPI_CMD_START_STOP_UNIT   0x1B
#define BELLATRIX_ATAPI_CMD_PREVENT_ALLOW     0x1E
#define BELLATRIX_ATAPI_CMD_READ_CAPACITY     0x25
#define BELLATRIX_ATAPI_CMD_READ_10           0x28
#define BELLATRIX_ATAPI_CMD_MODE_SENSE_6      0x1A

#define BELLATRIX_ATAPI_SENSE_NONE            0x00
#define BELLATRIX_ATAPI_SENSE_NOT_READY       0x02
#define BELLATRIX_ATAPI_SENSE_ILLEGAL_REQUEST 0x05
#define BELLATRIX_ATAPI_SENSE_UNIT_ATTENTION  0x06

typedef struct BellatrixAtapiSense {
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
} BellatrixAtapiSense;

typedef struct BellatrixAtapiResult {
    bool irq;
    bool check_condition;
    uint32_t transfer_length;
    const uint8_t *transfer_buffer;
} BellatrixAtapiResult;

typedef struct BellatrixAtapiCdrom {
    BellatrixCdromDevice *device;
    BellatrixAtapiSense sense;
    uint8_t data_buffer[64 * 1024];
} BellatrixAtapiCdrom;

void bellatrix_atapi_cdrom_init(
    BellatrixAtapiCdrom *atapi,
    BellatrixCdromDevice *device
);
void bellatrix_atapi_cdrom_reset(BellatrixAtapiCdrom *atapi);
void bellatrix_atapi_cdrom_set_sense(
    BellatrixAtapiCdrom *atapi,
    uint8_t key,
    uint8_t asc,
    uint8_t ascq
);
void bellatrix_atapi_cdrom_clear_sense(BellatrixAtapiCdrom *atapi);
bool bellatrix_atapi_cdrom_execute_packet(
    BellatrixAtapiCdrom *atapi,
    const uint8_t *cdb,
    size_t cdb_len,
    BellatrixAtapiResult *result
);

#endif
