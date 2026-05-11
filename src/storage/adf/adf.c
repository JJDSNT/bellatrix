#include "storage/adf/adf.h"

#include <string.h>

static size_t file_size(FILE *fp)
{
    long current = ftell(fp);

    if (current < 0) {
        return 0;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        return 0;
    }

    long end = ftell(fp);

    fseek(fp, current, SEEK_SET);

    if (end < 0) {
        return 0;
    }

    return (size_t)end;
}

bool adf_open(ADFImage *adf,
              const char *path,
              bool write_protected)
{
    if (!adf || !path) {
        return false;
    }

    memset(adf, 0, sizeof(*adf));

    strncpy(adf->path,
            path,
            sizeof(adf->path) - 1);

    adf->write_protected =
        write_protected;

    const char *mode =
        write_protected
            ? "rb"
            : "r+b";

    adf->fp = fopen(path, mode);

    /*
     * Fallback:
     * readonly open.
     */

    if (!adf->fp && !write_protected) {

        adf->fp = fopen(path, "rb");

        if (adf->fp) {
            adf->write_protected = true;
        }
    }

    if (!adf->fp) {
        return false;
    }

    adf->size =
        file_size(adf->fp);

    adf->loaded = true;

    return true;
}

void adf_close(ADFImage *adf)
{
    if (!adf) {
        return;
    }

    if (adf->fp) {
        fclose(adf->fp);
    }

    memset(adf, 0, sizeof(*adf));
}

bool adf_is_loaded(const ADFImage *adf)
{
    return adf && adf->loaded;
}

size_t adf_size(const ADFImage *adf)
{
    if (!adf) {
        return 0;
    }

    return adf->size;
}

bool adf_valid_track(uint32_t cylinder,
                     uint32_t side)
{
    if (cylinder >= 80) {
        return false;
    }

    if (side > 1) {
        return false;
    }

    return true;
}

size_t adf_track_offset(uint32_t cylinder,
                        uint32_t side)
{
    uint32_t track =
        (cylinder * 2u) + side;

    return (size_t)track *
           ADF_TRACK_SIZE;
}

bool adf_read_track(ADFImage *adf,
                    uint32_t cylinder,
                    uint32_t side,
                    void *buffer,
                    size_t buffer_size)
{
    if (!adf || !buffer) {
        return false;
    }

    if (!adf->loaded) {
        return false;
    }

    if (!adf_valid_track(cylinder, side)) {
        return false;
    }

    if (buffer_size < ADF_TRACK_SIZE) {
        return false;
    }

    size_t offset =
        adf_track_offset(
            cylinder,
            side);

    if (fseek(adf->fp,
              (long)offset,
              SEEK_SET) != 0)
    {
        return false;
    }

    size_t n =
        fread(buffer,
              1,
              ADF_TRACK_SIZE,
              adf->fp);

    return n == ADF_TRACK_SIZE;
}

bool adf_write_track(ADFImage *adf,
                     uint32_t cylinder,
                     uint32_t side,
                     const void *buffer,
                     size_t buffer_size)
{
    if (!adf || !buffer) {
        return false;
    }

    if (!adf->loaded ||
        adf->write_protected)
    {
        return false;
    }

    if (!adf_valid_track(cylinder, side)) {
        return false;
    }

    if (buffer_size < ADF_TRACK_SIZE) {
        return false;
    }

    size_t offset =
        adf_track_offset(
            cylinder,
            side);

    if (fseek(adf->fp,
              (long)offset,
              SEEK_SET) != 0)
    {
        return false;
    }

    size_t n =
        fwrite(buffer,
               1,
               ADF_TRACK_SIZE,
               adf->fp);

    fflush(adf->fp);

    return n == ADF_TRACK_SIZE;
}

bool adf_read_sector(ADFImage *adf,
                     uint32_t cylinder,
                     uint32_t side,
                     uint32_t sector,
                     void *buffer,
                     size_t buffer_size)
{
    if (!adf || !buffer) {
        return false;
    }

    if (sector >= ADF_SECTORS_PER_TRACK) {
        return false;
    }

    if (buffer_size < ADF_SECTOR_SIZE) {
        return false;
    }

    size_t offset =
        adf_track_offset(
            cylinder,
            side);

    offset +=
        sector *
        ADF_SECTOR_SIZE;

    if (fseek(adf->fp,
              (long)offset,
              SEEK_SET) != 0)
    {
        return false;
    }

    size_t n =
        fread(buffer,
              1,
              ADF_SECTOR_SIZE,
              adf->fp);

    return n == ADF_SECTOR_SIZE;
}

bool adf_write_sector(ADFImage *adf,
                      uint32_t cylinder,
                      uint32_t side,
                      uint32_t sector,
                      const void *buffer,
                      size_t buffer_size)
{
    if (!adf || !buffer) {
        return false;
    }

    if (!adf->loaded ||
        adf->write_protected)
    {
        return false;
    }

    if (sector >= ADF_SECTORS_PER_TRACK) {
        return false;
    }

    if (buffer_size < ADF_SECTOR_SIZE) {
        return false;
    }

    size_t offset =
        adf_track_offset(
            cylinder,
            side);

    offset +=
        sector *
        ADF_SECTOR_SIZE;

    if (fseek(adf->fp,
              (long)offset,
              SEEK_SET) != 0)
    {
        return false;
    }

    size_t n =
        fwrite(buffer,
               1,
               ADF_SECTOR_SIZE,
               adf->fp);

    fflush(adf->fp);

    return n == ADF_SECTOR_SIZE;
}