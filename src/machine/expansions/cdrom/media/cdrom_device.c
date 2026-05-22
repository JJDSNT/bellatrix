#include "machine/expansions/cdrom/media/cdrom_device.h"

#include <string.h>

void bellatrix_cdrom_device_init(BellatrixCdromDevice *cdrom)
{
    if (!cdrom) {
        return;
    }

    memset(cdrom, 0, sizeof(*cdrom));
}

void bellatrix_cdrom_device_attach_media(
    BellatrixCdromDevice *cdrom,
    const BellatrixCdromMediaOps *media
)
{
    if (!cdrom || !media) {
        return;
    }

    cdrom->media = *media;
    cdrom->media_changed = true;
}

void bellatrix_cdrom_device_eject(BellatrixCdromDevice *cdrom)
{
    if (!cdrom) {
        return;
    }

    cdrom->tray_open = true;
    cdrom->media_changed = true;
}

void bellatrix_cdrom_device_insert(BellatrixCdromDevice *cdrom)
{
    if (!cdrom) {
        return;
    }

    cdrom->tray_open = false;
    cdrom->media_changed = true;
}

bool bellatrix_cdrom_device_has_media(const BellatrixCdromDevice *cdrom)
{
    if (!cdrom || cdrom->tray_open || !cdrom->media.present) {
        return false;
    }

    return cdrom->media.present(cdrom->media.opaque);
}

bool bellatrix_cdrom_device_media_changed(const BellatrixCdromDevice *cdrom)
{
    return cdrom ? cdrom->media_changed : false;
}

void bellatrix_cdrom_device_clear_media_changed(BellatrixCdromDevice *cdrom)
{
    if (!cdrom) {
        return;
    }

    cdrom->media_changed = false;
}

uint32_t bellatrix_cdrom_device_sector_count(const BellatrixCdromDevice *cdrom)
{
    if (!cdrom || !bellatrix_cdrom_device_has_media(cdrom) ||
        !cdrom->media.sector_count) {
        return 0;
    }

    return cdrom->media.sector_count(cdrom->media.opaque);
}

BellatrixCdromStatus bellatrix_cdrom_device_read(
    BellatrixCdromDevice *cdrom,
    uint32_t lba,
    uint32_t count,
    uint8_t *buffer,
    size_t buffer_size
)
{
    uint32_t sectors;

    if (!cdrom || !buffer) {
        return BELLATRIX_CDROM_STATUS_BAD_ARGUMENT;
    }

    if (!bellatrix_cdrom_device_has_media(cdrom)) {
        return BELLATRIX_CDROM_STATUS_NO_MEDIA;
    }

    if (!cdrom->media.read_sectors) {
        return BELLATRIX_CDROM_STATUS_IO_ERROR;
    }

    sectors = bellatrix_cdrom_device_sector_count(cdrom);

    if (lba >= sectors || count > sectors - lba) {
        return BELLATRIX_CDROM_STATUS_INVALID_LBA;
    }

    if (buffer_size < ((size_t)count * BELLATRIX_CDROM_SECTOR_SIZE)) {
        return BELLATRIX_CDROM_STATUS_BAD_ARGUMENT;
    }

    cdrom->last_lba = lba;
    cdrom->last_count = count;

    return cdrom->media.read_sectors(
        cdrom->media.opaque,
        lba,
        count,
        buffer,
        buffer_size
    );
}
