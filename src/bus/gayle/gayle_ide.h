// src/bus/gayle/gayle_ide.h

#ifndef BELLATRIX_BUS_GAYLE_IDE_H
#define BELLATRIX_BUS_GAYLE_IDE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "bus/gayle/atapi_cd.h"

#define GAYLE_IDE_REG_DATA      0u
#define GAYLE_IDE_REG_ERROR     1u
#define GAYLE_IDE_REG_FEATURES  1u
#define GAYLE_IDE_REG_SECCNT    2u
#define GAYLE_IDE_REG_SECNUM    3u
#define GAYLE_IDE_REG_CYLLO     4u
#define GAYLE_IDE_REG_CYLHI     5u
#define GAYLE_IDE_REG_DEVHEAD   6u
#define GAYLE_IDE_REG_STATUS    7u
#define GAYLE_IDE_REG_COMMAND   7u

#define ATA_STATUS_ERR   0x01u
#define ATA_STATUS_DRQ   0x08u
#define ATA_STATUS_DF    0x20u
#define ATA_STATUS_DRDY  0x40u
#define ATA_STATUS_BSY   0x80u

#define ATA_CMD_DEVICE_RESET            0x08u
#define ATA_CMD_PACKET                  0xA0u
#define ATA_CMD_IDENTIFY_PACKET_DEVICE  0xA1u
#define ATA_CMD_IDENTIFY_DEVICE         0xECu

typedef struct GayleIde {
    AtapiCd cd;

    uint8_t status;
    uint8_t error;
    uint8_t features;
    uint8_t sector_count;
    uint8_t sector_number;
    uint8_t cyl_low;
    uint8_t cyl_high;
    uint8_t dev_head;

    uint8_t identify[512];
    size_t identify_pos;
    bool identify_active;
} GayleIde;

void gayle_ide_init(
    GayleIde *ide,
    IsoImage *iso,
    uint8_t *atapi_buffer,
    size_t atapi_buffer_size
);

void gayle_ide_reset(GayleIde *ide);

uint8_t gayle_ide_read8(GayleIde *ide, uint32_t reg);
void gayle_ide_write8(GayleIde *ide, uint32_t reg, uint8_t value);

uint16_t gayle_ide_read16(GayleIde *ide);
void gayle_ide_write16(GayleIde *ide, uint16_t value);

uint32_t gayle_ide_read32(GayleIde *ide);
void gayle_ide_write32(GayleIde *ide, uint32_t value);

bool gayle_ide_irq_pending(const GayleIde *ide);
void gayle_ide_clear_irq(GayleIde *ide);

#endif