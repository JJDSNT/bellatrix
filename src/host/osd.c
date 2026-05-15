// src/host/osd.c — on-screen FPS/frame overlay

#ifdef BELLATRIX_OSD

#include "host/osd.h"
#include "host/pal.h"
#include "support.h"

#include <stdint.h>

extern uint16_t *framebuffer;
extern uint32_t  pitch;
extern uint32_t  fb_width;
extern uint32_t  fb_height;

/* ------------------------------------------------------------------------- */
/* 8×8 bitmap font — characters needed for "FRM:nnnnnnnn FPS:nnn"           */
/* bit 7 = leftmost pixel of each row                                        */
/* ------------------------------------------------------------------------- */

/* indices: 0=space 1-10='0'-'9' 11=':' 12='F' 13='M' 14='P' 15='R' 16='S' */
static const uint8_t osd_font[17][8] = {
    { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 }, /*  0  space */
    { 0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00 }, /*  1  '0'   */
    { 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 }, /*  2  '1'   */
    { 0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00 }, /*  3  '2'   */
    { 0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00 }, /*  4  '3'   */
    { 0x0C,0x1C,0x2C,0x4C,0x7E,0x0C,0x0C,0x00 }, /*  5  '4'   */
    { 0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00 }, /*  6  '5'   */
    { 0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00 }, /*  7  '6'   */
    { 0x7E,0x06,0x0C,0x18,0x18,0x18,0x18,0x00 }, /*  8  '7'   */
    { 0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00 }, /*  9  '8'   */
    { 0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00 }, /* 10  '9'   */
    { 0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00 }, /* 11  ':'   */
    { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00 }, /* 12  'F'   */
    { 0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00 }, /* 13  'M'   */
    { 0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00 }, /* 14  'P'   */
    { 0x7C,0x66,0x66,0x7C,0x6C,0x66,0x63,0x00 }, /* 15  'R'   */
    { 0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00 }, /* 16  'S'   */
};

static uint8_t osd_char_idx(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(1 + (c - '0'));
    switch (c) {
    case ':': return 11;
    case 'F': return 12;
    case 'M': return 13;
    case 'P': return 14;
    case 'R': return 15;
    case 'S': return 16;
    default:  return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* FPS state                                                                 */
/* ------------------------------------------------------------------------- */

static uint64_t osd_tick0   = 0;
static uint64_t osd_frame0  = 0;
static uint32_t osd_fps     = 0;

static void osd_update_fps(uint64_t frame)
{
    uint64_t now  = PAL_Time_ReadCounter();
    uint64_t freq = PAL_Time_GetFrequency();

    if (osd_tick0 == 0) {
        osd_tick0  = now;
        osd_frame0 = frame;
        return;
    }

    uint64_t df = frame - osd_frame0;
    if (df < 32)
        return;

    uint64_t dt = now - osd_tick0;
    if (dt > 0 && freq > 0)
        osd_fps = (uint32_t)(df * freq / dt);

    osd_tick0  = now;
    osd_frame0 = frame;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

static int osd_u64_to_str(char *buf, uint64_t val, int min_digits)
{
    char tmp[24];
    int len = 0;

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0) {
            tmp[len++] = (char)('0' + (int)(val % 10));
            val /= 10;
        }
    }

    while (len < min_digits)
        tmp[len++] = '0';

    for (int i = 0; i < len; i++)
        buf[i] = tmp[len - 1 - i];

    buf[len] = '\0';
    return len;
}

static void osd_putchar(uint16_t *fb, uint32_t stride, int x, int y,
                        char c, uint16_t color)
{
    const uint8_t *glyph = osd_font[osd_char_idx(c)];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (uint8_t)(0x80u >> col))
                fb[(y + row) * stride + (x + col)] = color;
        }
    }
}

void osd_render(uint64_t frame)
{
    if (!framebuffer || fb_width < 100 || fb_height < 20)
        return;

    osd_update_fps(frame);

    /* build string "FRM:00000000 FPS:000" */
    char buf[32];
    int  pos = 0;
    buf[pos++] = 'F'; buf[pos++] = 'R'; buf[pos++] = 'M'; buf[pos++] = ':';
    pos += osd_u64_to_str(buf + pos, frame, 8);
    buf[pos++] = ' ';
    buf[pos++] = 'F'; buf[pos++] = 'P'; buf[pos++] = 'S'; buf[pos++] = ':';
    pos += osd_u64_to_str(buf + pos, (uint64_t)osd_fps, 3);
    buf[pos] = '\0';

    int sx = 4;
    int sy = 4;
    int text_w = pos * 8;
    int pad    = 2;

    uint32_t stride = pitch / 2u;  /* pitch is bytes; each pixel = 2 bytes */

    /* black background rectangle */
    for (int row = sy - pad; row < sy + 8 + pad; row++) {
        uint16_t *line = framebuffer + (uint32_t)row * stride + (uint32_t)(sx - pad);
        for (int col = 0; col < text_w + 2 * pad; col++)
            line[col] = LE16(0x0000u);
    }

    /* white text */
    uint16_t white = LE16(0xFFFFu);
    for (int i = 0; i < pos; i++)
        osd_putchar(framebuffer, stride, sx + i * 8, sy, buf[i], white);
}

#endif /* BELLATRIX_OSD */
