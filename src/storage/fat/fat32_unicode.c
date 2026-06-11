#include "fat32_unicode.h"

static int put_utf8(char **out, size_t *left, uint32_t cp)
{
    if (cp <= 0x7F) {
        if (*left < 2) return -1;
        *(*out)++ = (char)cp;
        (*left)--;
    } else if (cp <= 0x7FF) {
        if (*left < 3) return -1;
        *(*out)++ = (char)(0xC0 | (cp >> 6));
        *(*out)++ = (char)(0x80 | (cp & 0x3F));
        (*left) -= 2;
    } else if (cp <= 0xFFFF) {
        if (*left < 4) return -1;
        *(*out)++ = (char)(0xE0 | (cp >> 12));
        *(*out)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*out)++ = (char)(0x80 | (cp & 0x3F));
        (*left) -= 3;
    } else {
        if (*left < 5) return -1;
        *(*out)++ = (char)(0xF0 | (cp >> 18));
        *(*out)++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *(*out)++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *(*out)++ = (char)(0x80 | (cp & 0x3F));
        (*left) -= 4;
    }

    return 0;
}

int fat32_utf16le_to_utf8(
    const uint16_t *in,
    size_t in_len,
    char *out,
    size_t out_cap
) {
    char *p = out;
    size_t left = out_cap;

    if (!in || !out || out_cap == 0) return -1;

    for (size_t i = 0; i < in_len; i++) {
        uint32_t ch = in[i];

        if (ch == 0x0000 || ch == 0xFFFF) break;

        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (i + 1 >= in_len) return -1;

            uint32_t lo = in[++i];
            if (lo < 0xDC00 || lo > 0xDFFF) return -1;

            ch = 0x10000 + (((ch - 0xD800) << 10) | (lo - 0xDC00));
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            return -1;
        }

        if (put_utf8(&p, &left, ch) != 0)
            return -1;
    }

    if (left == 0) return -1;
    *p = '\0';

    return 0;
}