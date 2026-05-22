// src/storage/iso/iso_image.c

#include "storage/iso/iso_image.h"

#include <string.h>

void iso_image_init(IsoImage *iso)
{
    if (!iso)
        return;

    iso->data         = NULL;
    iso->read_fn      = NULL;
    iso->read_ctx     = NULL;
    iso->sector_count = 0;
    iso->present      = false;
}

bool iso_image_attach(IsoImage *iso, const void *data, size_t size)
{
    if (!iso || !data || size < ISO_SECTOR_SIZE)
        return false;

    iso->data         = (const uint8_t *)data;
    iso->read_fn      = NULL;
    iso->read_ctx     = NULL;
    iso->sector_count = (uint32_t)(size / ISO_SECTOR_SIZE);
    iso->present      = iso->sector_count > 0;

    return iso->present;
}

bool iso_image_attach_fn(IsoImage   *iso,
                         iso_read_fn fn,
                         void       *ctx,
                         uint32_t    sector_count)
{
    if (!iso || !fn || sector_count == 0)
        return false;

    iso->data         = NULL;
    iso->read_fn      = fn;
    iso->read_ctx     = ctx;
    iso->sector_count = sector_count;
    iso->present      = true;

    return true;
}

void iso_image_detach(IsoImage *iso)
{
    iso_image_init(iso);
}

bool iso_image_present(const IsoImage *iso)
{
    return iso && iso->present && iso->sector_count > 0 &&
           (iso->data != NULL || iso->read_fn != NULL);
}

uint32_t iso_image_sector_count(const IsoImage *iso)
{
    if (!iso_image_present(iso))
        return 0;

    return iso->sector_count;
}

bool iso_image_read_sector(
    const IsoImage *iso,
    uint32_t lba,
    void *dst
)
{
    if (!iso_image_present(iso) || !dst)
        return false;

    if (lba >= iso->sector_count)
        return false;

    if (iso->read_fn)
        return iso->read_fn(iso->read_ctx, lba, 1, dst);

    memcpy(dst, iso->data + ((size_t)lba * ISO_SECTOR_SIZE), ISO_SECTOR_SIZE);
    return true;
}

bool iso_image_read_sectors(
    const IsoImage *iso,
    uint32_t lba,
    uint32_t count,
    void *dst
)
{
    if (!iso_image_present(iso) || !dst)
        return false;

    if (count == 0)
        return true;

    if (lba >= iso->sector_count)
        return false;

    if (count > iso->sector_count - lba)
        return false;

    if (iso->read_fn)
        return iso->read_fn(iso->read_ctx, lba, count, dst);

    memcpy(dst, iso->data + ((size_t)lba * ISO_SECTOR_SIZE),
           (size_t)count * ISO_SECTOR_SIZE);

    return true;
}
