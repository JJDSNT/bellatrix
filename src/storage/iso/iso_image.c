// src/storage/iso/iso_image.c

#include "storage/iso/iso_image.h"

#include <string.h>

void iso_image_init(IsoImage *iso)
{
    if (!iso)
        return;

    iso->data = NULL;
    iso->size = 0;
    iso->sector_count = 0;
    iso->present = false;
}

bool iso_image_attach(IsoImage *iso, const void *data, size_t size)
{
    if (!iso || !data || size < ISO_SECTOR_SIZE)
        return false;

    iso->data = (const uint8_t *)data;
    iso->size = size;
    iso->sector_count = (uint32_t)(size / ISO_SECTOR_SIZE);
    iso->present = iso->sector_count > 0;

    return iso->present;
}

void iso_image_detach(IsoImage *iso)
{
    iso_image_init(iso);
}

bool iso_image_present(const IsoImage *iso)
{
    return iso && iso->present && iso->data && iso->sector_count > 0;
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
    uint8_t *out = (uint8_t *)dst;

    if (!iso_image_present(iso) || !dst)
        return false;

    if (count == 0)
        return true;

    if (lba >= iso->sector_count)
        return false;

    if (count > iso->sector_count - lba)
        return false;

    memcpy(out, iso->data + ((size_t)lba * ISO_SECTOR_SIZE),
           (size_t)count * ISO_SECTOR_SIZE);

    return true;
}