#ifndef BELLATRIX_FLOPPY_MFM_H
#define BELLATRIX_FLOPPY_MFM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AMIGA_SECTOR_SIZE          512
#define AMIGA_SECTORS_PER_TRACK    11

#define AMIGA_MFM_SECTOR_SIZE      1088
#define AMIGA_MFM_TRACK_SIZE \
    (AMIGA_SECTOR_SIZE * AMIGA_SECTORS_PER_TRACK * 2)

/*
 * Encoded MFM sector.
 */
typedef struct AmigaMFMSector {
    uint8_t data[AMIGA_MFM_SECTOR_SIZE];
    size_t size;
} AmigaMFMSector;

/*
 * Encoded MFM track.
 */
typedef struct AmigaMFMTrack {
    uint8_t *data;
    size_t size;
} AmigaMFMTrack;

/*
 * Helpers.
 */

uint32_t floppy_mfm_checksum(
    const uint32_t *data,
    size_t longwords);

uint32_t floppy_mfm_encode_even_odd(
    uint32_t value,
    uint32_t *even,
    uint32_t *odd);

uint16_t floppy_mfm_encode_word(
    uint16_t raw,
    uint16_t previous);

/*
 * Sector encoding.
 */

bool floppy_mfm_encode_sector(
    uint8_t *dst,
    size_t dst_size,

    const uint8_t *sector_data,
    uint32_t track,
    uint32_t sector,
    uint32_t sectors_per_track);

/*
 * Track encoding.
 */

bool floppy_mfm_encode_track(
    uint8_t *dst,
    size_t dst_size,

    const uint8_t *track_data,
    uint32_t track,
    uint32_t sectors_per_track);

#endif