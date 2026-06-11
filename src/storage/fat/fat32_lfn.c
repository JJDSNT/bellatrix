#include "fat32_lfn.h"
#include "fat32_unicode.h"

#include <string.h>

static uint16_t rd16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

void fat32_lfn_reset(fat32_lfn_state_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

uint8_t fat32_lfn_checksum(const uint8_t short_name[11])
{
    uint8_t sum = 0;

    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + short_name[i]);
    }

    return sum;
}

static void read_lfn_chars(const uint8_t ent[32], uint16_t out[13])
{
    out[0]  = rd16le(&ent[1]);
    out[1]  = rd16le(&ent[3]);
    out[2]  = rd16le(&ent[5]);
    out[3]  = rd16le(&ent[7]);
    out[4]  = rd16le(&ent[9]);

    out[5]  = rd16le(&ent[14]);
    out[6]  = rd16le(&ent[16]);
    out[7]  = rd16le(&ent[18]);
    out[8]  = rd16le(&ent[20]);
    out[9]  = rd16le(&ent[22]);
    out[10] = rd16le(&ent[24]);

    out[11] = rd16le(&ent[28]);
    out[12] = rd16le(&ent[30]);
}

int fat32_lfn_push_entry(fat32_lfn_state_t *s, const uint8_t ent[32])
{
    uint8_t ord;
    uint8_t seq;
    uint16_t chars[13];

    if (!s || !ent) return -1;

    ord = ent[0];
    seq = ord & 0x1F;

    if (seq == 0 || seq > 20) {
        fat32_lfn_reset(s);
        return -1;
    }

    if (ord & 0x40) {
        fat32_lfn_reset(s);
        s->active = 1;
        s->valid = 1;
        s->expected_ord = seq;
        s->checksum = ent[13];
    } else {
        if (!s->active || !s->valid) {
            fat32_lfn_reset(s);
            return -1;
        }

        if (ent[13] != s->checksum || seq + 1 != s->expected_ord) {
            fat32_lfn_reset(s);
            return -1;
        }

        s->expected_ord = seq;
    }

    read_lfn_chars(ent, chars);

    size_t offset = (size_t)(seq - 1) * 13;

    if (offset + 13 > FAT32_LFN_MAX_UTF16) {
        fat32_lfn_reset(s);
        return -1;
    }

    for (size_t i = 0; i < 13; i++) {
        s->name[offset + i] = chars[i];

        if (chars[i] == 0x0000) {
            size_t end = offset + i;
            if (end > s->len) s->len = end;
            return 0;
        }
    }

    if (offset + 13 > s->len)
        s->len = offset + 13;

    return 0;
}

int fat32_lfn_finish(
    fat32_lfn_state_t *s,
    const uint8_t short_ent[32],
    char *out,
    size_t out_cap
) {
    int rc;

    if (!s || !short_ent || !out || out_cap == 0)
        return -1;

    if (!s->active || !s->valid || s->len == 0)
        return -1;

    if (s->checksum != fat32_lfn_checksum(short_ent))
        return -1;

    rc = fat32_utf16le_to_utf8(s->name, s->len, out, out_cap);
    fat32_lfn_reset(s);

    return rc;
}

void fat32_short_name_to_string(
    const uint8_t ent[32],
    char *out,
    size_t out_cap
) {
    size_t p = 0;
    int base_end = 8;
    int ext_end = 11;

    if (!ent || !out || out_cap == 0)
        return;

    /* NT reserved byte: Windows stores all-lowercase 8.3 names without an
       LFN, flagging case here instead (0x08 = lowercase base, 0x10 =
       lowercase extension). */
    int lc_base = ent[12] & 0x08;
    int lc_ext  = ent[12] & 0x10;

    while (base_end > 0 && ent[base_end - 1] == ' ')
        base_end--;

    while (ext_end > 8 && ent[ext_end - 1] == ' ')
        ext_end--;

    for (int i = 0; i < base_end && p + 1 < out_cap; i++) {
        char c = (char)ent[i];
        if (lc_base && c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        out[p++] = c;
    }

    if (ext_end > 8 && p + 1 < out_cap)
        out[p++] = '.';

    for (int i = 8; i < ext_end && p + 1 < out_cap; i++) {
        char c = (char)ent[i];
        if (lc_ext && c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        out[p++] = c;
    }

    out[p] = '\0';
}