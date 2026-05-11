#ifndef BELLATRIX_STORAGE_ADF_H
#define BELLATRIX_STORAGE_ADF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#define ADF_TRACKS             160
#define ADF_SECTORS_PER_TRACK  11
#define ADF_SECTOR_SIZE        512

#define ADF_TRACK_SIZE \
    (ADF_SECTORS_PER_TRACK * ADF_SECTOR_SIZE)

#define ADF_DD_SIZE \
    (ADF_TRACKS * ADF_TRACK_SIZE)

typedef struct ADFImage {
    FILE *fp;

    char path[512];

    size_t size;

    bool write_protected;
    bool loaded;
} ADFImage;

/*
 * Lifecycle.
 */

bool adf_open(ADFImage *adf,
              const char *path,
              bool write_protected);

void adf_close(ADFImage *adf);

/*
 * Status.
 */

bool adf_is_loaded(const ADFImage *adf);

size_t adf_size(const ADFImage *adf);

/*
 * Geometry helpers.
 */

size_t adf_track_offset(uint32_t cylinder,
                        uint32_t side);

bool adf_valid_track(uint32_t cylinder,
                     uint32_t side);

/*
 * Raw track access.
 */

bool adf_read_track(ADFImage *adf,
                    uint32_t cylinder,
                    uint32_t side,
                    void *buffer,
                    size_t buffer_size);

bool adf_write_track(ADFImage *adf,
                     uint32_t cylinder,
                     uint32_t side,
                     const void *buffer,
                     size_t buffer_size);

/*
 * Sector access.
 */

bool adf_read_sector(ADFImage *adf,
                     uint32_t cylinder,
                     uint32_t side,
                     uint32_t sector,
                     void *buffer,
                     size_t buffer_size);

bool adf_write_sector(ADFImage *adf,
                      uint32_t cylinder,
                      uint32_t side,
                      uint32_t sector,
                      const void *buffer,
                      size_t buffer_size);

#endif