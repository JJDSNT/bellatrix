#ifndef BELLATRIX_EXPANSIONS_CDROM_MEDIA_CDROM_ISO_BACKEND_H
#define BELLATRIX_EXPANSIONS_CDROM_MEDIA_CDROM_ISO_BACKEND_H

#include "machine/expansions/cdrom/media/cdrom_device.h"
#include "storage/iso/iso_image.h"

typedef struct BellatrixCdromIsoBackend {
    IsoImage *iso;
} BellatrixCdromIsoBackend;

void bellatrix_cdrom_iso_backend_init(
    BellatrixCdromIsoBackend *backend,
    IsoImage *iso
);

void bellatrix_cdrom_iso_backend_get_ops(
    BellatrixCdromIsoBackend *backend,
    BellatrixCdromMediaOps *ops
);

#endif
