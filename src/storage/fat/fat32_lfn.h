#ifndef FAT32_LFN_H
#define FAT32_LFN_H

#include <stddef.h>
#include <stdint.h>

#define FAT32_ATTR_LFN 0x0F
#define FAT32_LFN_MAX_UTF16 260
#define FAT32_LFN_MAX_UTF8  1024

typedef struct fat32_lfn_state {
    uint16_t name[FAT32_LFN_MAX_UTF16];
    size_t len;
    uint8_t checksum;
    uint8_t expected_ord;
    int active;
    int valid;
} fat32_lfn_state_t;

void fat32_lfn_reset(fat32_lfn_state_t *s);

int fat32_lfn_push_entry(
    fat32_lfn_state_t *s,
    const uint8_t dirent[32]
);

int fat32_lfn_finish(
    fat32_lfn_state_t *s,
    const uint8_t short_ent[32],
    char *out,
    size_t out_cap
);

void fat32_short_name_to_string(
    const uint8_t short_ent[32],
    char *out,
    size_t out_cap
);

uint8_t fat32_lfn_checksum(const uint8_t short_name[11]);

#endif