// src/storage/iso/iso_image.h

#ifndef BELLATRIX_STORAGE_ISO_IMAGE_H
#define BELLATRIX_STORAGE_ISO_IMAGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ISO_SECTOR_SIZE 2048u

/*
 * Sector read callback for on-demand (non-preloaded) ISO access.
 * Returns true on success, false on I/O error or out-of-range LBA.
 */
typedef bool (*iso_read_fn)(void *ctx, uint32_t lba, uint32_t count, void *dst);

typedef struct IsoImage {
    /*
     * In-memory path: data != NULL, read_fn == NULL.
     * Callback path:  data == NULL, read_fn != NULL.
     * Only one is active at a time.
     */
    const uint8_t *data;       // non-NULL for in-memory images
    iso_read_fn    read_fn;    // non-NULL for callback-based images
    void          *read_ctx;
    uint32_t       sector_count;
    bool           present;
} IsoImage;

void iso_image_init(IsoImage *iso);

/* Attach an in-memory ISO image (harness / QEMU loader). */
bool iso_image_attach(IsoImage *iso, const void *data, size_t size);

/*
 * Attach a callback-based ISO image (bare-metal SD card reader).
 * sector_count must be > 0; fn must not be NULL.
 */
bool iso_image_attach_fn(IsoImage   *iso,
                         iso_read_fn fn,
                         void       *ctx,
                         uint32_t    sector_count);

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