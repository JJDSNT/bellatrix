#ifndef FAT32_UNICODE_H
#define FAT32_UNICODE_H

#include <stddef.h>
#include <stdint.h>

int fat32_utf16le_to_utf8(
    const uint16_t *in,
    size_t in_len,
    char *out,
    size_t out_cap
);

#endif