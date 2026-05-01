// src/storage/iso/iso_image.h

#ifndef BELLATRIX_STORAGE_ISO_IMAGE_H
#define BELLATRIX_STORAGE_ISO_IMAGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ISO_SECTOR_SIZE 2048u

typedef struct IsoImage {
    const uint8_t *data;
    size_t size;
    uint32_t sector_count;
    bool present;
} IsoImage;

void iso_image_init(IsoImage *iso);

bool iso_image_attach(IsoImage *iso, const void *data, size_t size);

void iso_image_detach(IsoImage *iso);

bool iso_image_present(const IsoImage *iso);

uint32_t iso_image_sector_count(const IsoImage *iso);

bool iso_image_read_sector(
    const IsoImage *iso,
    uint32_t lba,
    void *dst
);

bool iso_image_read_sectors(
    const IsoImage *iso,
    uint32_t lba,
    uint32_t count,
    void *dst
);

#endif