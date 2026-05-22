#include "machine/expansions/cdrom/media/cdrom_iso_backend.h"

#include <string.h>

static bool bellatrix_cdrom_iso_present(void *opaque)
{
    BellatrixCdromIsoBackend *backend;

    backend = (BellatrixCdromIsoBackend *)opaque;

    return backend && backend->iso && iso_image_present(backend->iso);
}

static uint32_t bellatrix_cdrom_iso_sector_count(void *opaque)
{
    BellatrixCdromIsoBackend *backend;

    backend = (BellatrixCdromIsoBackend *)opaque;
    if (!backend || !backend->iso) {
        return 0;
    }

    return iso_image_sector_count(backend->iso);
}

static BellatrixCdromStatus bellatrix_cdrom_iso_read_sectors(
    void *opaque,
    uint32_t lba,
    uint32_t count,
    uint8_t *buffer,
    size_t buffer_size
)
{
    BellatrixCdromIsoBackend *backend;

    backend = (BellatrixCdromIsoBackend *)opaque;

    if (!backend || !backend->iso || !buffer) {
        return BELLATRIX_CDROM_STATUS_BAD_ARGUMENT;
    }

    if (buffer_size < ((size_t)count * BELLATRIX_CDROM_SECTOR_SIZE)) {
        return BELLATRIX_CDROM_STATUS_BAD_ARGUMENT;
    }

    if (!iso_image_read_sectors(backend->iso, lba, count, buffer)) {
        return BELLATRIX_CDROM_STATUS_IO_ERROR;
    }

    return BELLATRIX_CDROM_STATUS_OK;
}

void bellatrix_cdrom_iso_backend_init(
    BellatrixCdromIsoBackend *backend,
    IsoImage *iso
)
{
    if (!backend) {
        return;
    }

    memset(backend, 0, sizeof(*backend));
    backend->iso = iso;
}

void bellatrix_cdrom_iso_backend_get_ops(
    BellatrixCdromIsoBackend *backend,
    BellatrixCdromMediaOps *ops
)
{
    if (!backend || !ops) {
        return;
    }

    memset(ops, 0, sizeof(*ops));
    ops->opaque = backend;
    ops->present = bellatrix_cdrom_iso_present;
    ops->sector_count = bellatrix_cdrom_iso_sector_count;
    ops->read_sectors = bellatrix_cdrom_iso_read_sectors;
}
