#include "chipset/floppy/floppy_mfm.h"

#include <string.h>

#define MFM_MASK 0x55555555u

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xff);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

uint32_t floppy_mfm_checksum(
    const uint32_t *data,
    size_t longwords)
{
    uint32_t sum = 0;

    if (!data) {
        return 0;
    }

    for (size_t i = 0; i < longwords; i++) {
        sum ^= (data[i] & MFM_MASK);
    }

    return sum;
}

uint32_t floppy_mfm_encode_even_odd(
    uint32_t value,
    uint32_t *even,
    uint32_t *odd)
{
    uint32_t e =
        (value >> 1) & MFM_MASK;

    uint32_t o =
        value & MFM_MASK;

    if (even) {
        *even = e;
    }

    if (odd) {
        *odd = o;
    }

    return 0;
}

uint16_t floppy_mfm_encode_word(
    uint16_t raw,
    uint16_t previous)
{
    uint16_t out = 0;

    for (int i = 15; i >= 0; i--) {

        uint16_t bit =
            (raw >> i) & 1u;

        uint16_t prev_data =
            (i == 15)
                ? (previous & 1u)
                : ((raw >> (i + 1)) & 1u);

        uint16_t clock =
            (!prev_data && !bit) ? 1u : 0u;

        out <<= 1;
        out |= clock;

        out <<= 1;
        out |= bit;
    }

    return out;
}

static void write_sync_words(uint8_t *dst)
{
    /*
     * Standard Amiga sync.
     */

    for (int i = 0; i < 4; i++) {
        dst[i * 2 + 0] = 0x44;
        dst[i * 2 + 1] = 0x89;
    }
}

bool floppy_mfm_encode_sector(
    uint8_t *dst,
    size_t dst_size,

    const uint8_t *sector_data,
    uint32_t track,
    uint32_t sector,
    uint32_t sectors_per_track)
{
    if (!dst || !sector_data) {
        return false;
    }

    if (dst_size < AMIGA_MFM_SECTOR_SIZE) {
        return false;
    }

    memset(dst, 0, AMIGA_MFM_SECTOR_SIZE);

    uint8_t *p = dst;

    /*
     * Sync words.
     */

    write_sync_words(p);
    p += 8;

    /*
     * Header.
     */

    uint32_t header =
        ((track & 0xffu) << 24) |
        ((sector & 0xffu) << 16) |
        (((sectors_per_track - sector - 1) & 0xffu) << 8);

    uint32_t even, odd;

    floppy_mfm_encode_even_odd(
        header,
        &even,
        &odd);

    write_be32(p, even);
    p += 4;

    write_be32(p, odd);
    p += 4;

    /*
     * Header checksum.
     */

    uint32_t header_words[2] = {
        even,
        odd
    };

    uint32_t header_checksum =
        floppy_mfm_checksum(
            header_words,
            2);

    write_be32(p, header_checksum);
    p += 4;

    /*
     * Reserved gap.
     */

    memset(p, 0xaa, 16);
    p += 16;

    /*
     * Data encoding.
     */

    uint32_t encoded[256];

    for (int i = 0; i < 128; i++) {

        uint32_t raw =
            read_be32(
                &sector_data[i * 4]);

        uint32_t e, o;

        floppy_mfm_encode_even_odd(
            raw,
            &e,
            &o);

        encoded[i] = e;
        encoded[i + 128] = o;
    }

    /*
     * Data checksum.
     */

    uint32_t data_checksum =
        floppy_mfm_checksum(
            encoded,
            256);

    write_be32(p, data_checksum);
    p += 4;

    /*
     * Encoded data.
     */

    for (int i = 0; i < 256; i++) {
        write_be32(p, encoded[i]);
        p += 4;
    }

    return true;
}

bool floppy_mfm_encode_track(
    uint8_t *dst,
    size_t dst_size,

    const uint8_t *track_data,
    uint32_t track,
    uint32_t sectors_per_track)
{
    if (!dst || !track_data) {
        return false;
    }

    size_t required =
        sectors_per_track *
        AMIGA_MFM_SECTOR_SIZE;

    if (dst_size < required) {
        return false;
    }

    for (uint32_t sector = 0;
         sector < sectors_per_track;
         sector++)
    {
        const uint8_t *sector_ptr =
            &track_data[
                sector *
                AMIGA_SECTOR_SIZE];

        uint8_t *dst_ptr =
            &dst[
                sector *
                AMIGA_MFM_SECTOR_SIZE];

        if (!floppy_mfm_encode_sector(
                dst_ptr,
                AMIGA_MFM_SECTOR_SIZE,
                sector_ptr,
                track,
                sector,
                sectors_per_track))
        {
            return false;
        }
    }

    return true;
}