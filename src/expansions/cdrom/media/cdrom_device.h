#ifndef BELLATRIX_EXPANSIONS_CDROM_MEDIA_CDROM_DEVICE_H
#define BELLATRIX_EXPANSIONS_CDROM_MEDIA_CDROM_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BELLATRIX_CDROM_SECTOR_SIZE 2048u

typedef enum BellatrixCdromStatus {
    BELLATRIX_CDROM_STATUS_OK = 0,
    BELLATRIX_CDROM_STATUS_NO_MEDIA,
    BELLATRIX_CDROM_STATUS_INVALID_LBA,
    BELLATRIX_CDROM_STATUS_IO_ERROR,
    BELLATRIX_CDROM_STATUS_BAD_ARGUMENT
} BellatrixCdromStatus;

typedef struct BellatrixCdromMediaOps {
    void *opaque;
    bool (*present)(void *opaque);
    uint32_t (*sector_count)(void *opaque);
    BellatrixCdromStatus (*read_sectors)(
        void *opaque,
        uint32_t lba,
        uint32_t count,
        uint8_t *buffer,
        size_t buffer_size
    );
} BellatrixCdromMediaOps;

typedef struct BellatrixCdromDevice {
    BellatrixCdromMediaOps media;
    bool tray_open;
    bool media_changed;
    uint32_t last_lba;
    uint32_t last_count;
} BellatrixCdromDevice;

void bellatrix_cdrom_device_init(BellatrixCdromDevice *cdrom);
void bellatrix_cdrom_device_attach_media(
    BellatrixCdromDevice *cdrom,
    const BellatrixCdromMediaOps *media
);
void bellatrix_cdrom_device_eject(BellatrixCdromDevice *cdrom);
void bellatrix_cdrom_device_insert(BellatrixCdromDevice *cdrom);
bool bellatrix_cdrom_device_has_media(const BellatrixCdromDevice *cdrom);
bool bellatrix_cdrom_device_media_changed(const BellatrixCdromDevice *cdrom);
void bellatrix_cdrom_device_clear_media_changed(BellatrixCdromDevice *cdrom);
uint32_t bellatrix_cdrom_device_sector_count(const BellatrixCdromDevice *cdrom);
BellatrixCdromStatus bellatrix_cdrom_device_read(
    BellatrixCdromDevice *cdrom,
    uint32_t lba,
    uint32_t count,
    uint8_t *buffer,
    size_t buffer_size
);

#endif
