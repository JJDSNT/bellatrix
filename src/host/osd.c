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

/* indices: 0=space 1-10='0'-'9' 11=':' 12='F' 13='M' 14='P' 15='R' 16='S'
 * 17='U' 18='V' 19='!' 20='T' 21='%' */
static const uint8_t osd_font[22][8] = {
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
    { 0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 }, /* 17  'U'   */
    { 0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00 }, /* 18  'V'   */
    { 0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00 }, /* 19  '!'   */
    { 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 }, /* 20  'T'   */
    { 0x62,0x64,0x08,0x10,0x26,0x46,0x00,0x00 }, /* 21  '%'   */
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
    case 'U': return 17;
    case 'V': return 18;
    case '!': return 19;
    case 'T': return 20;
    case '%': return 21;
    default:  return 0;
    }
}

/* ------------------------------------------------------------------------- */
/* FPS state                                                                 */
/* ------------------------------------------------------------------------- */

static uint64_t osd_machine_frame = 0;
static uint32_t osd_realtime_percent = 0;

/* Written by the host heartbeat and read by the host presentation path. */
static uint32_t osd_throttled = 0;

void osd_set_power_alert(uint32_t throttled_flags)
{
    osd_throttled = throttled_flags;
}

void osd_set_machine_frame(uint64_t frame)
{
    osd_machine_frame = frame;
}

void osd_set_realtime_percent(uint32_t percent)
{
    osd_realtime_percent = percent > 999u ? 999u : percent;
}

/* ------------------------------------------------------------------------- */
/* Rendering                                                                 */
/* ------------------------------------------------------------------------- */

static int osd_u64_to_str(char *buf, int cap, uint64_t val, int min_digits)
{
    char tmp[20];
    int len = 0;

    if (cap <= 0)
        return 0;

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0 && len < (int)sizeof(tmp)) {
            tmp[len++] = (char)('0' + (int)(val % 10));
            val /= 10;
        }
    }

    while (len < min_digits && len < (int)sizeof(tmp))
        tmp[len++] = '0';

    int out_len = len;
    if (out_len >= cap)
        out_len = cap - 1;

    for (int i = 0; i < out_len; i++)
        buf[i] = tmp[len - 1 - i];

    buf[out_len] = '\0';
    return out_len;
}

static void osd_putchar(uint16_t *fb, uint32_t stride, int x, int y,
                        char c, uint16_t color, uint32_t scale)
{
    const uint8_t *glyph = osd_font[osd_char_idx(c)];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (uint8_t)(0x80u >> col)) {
                for (uint32_t sy2 = 0; sy2 < scale; sy2++)
                    for (uint32_t sx2 = 0; sx2 < scale; sx2++)
                        fb[((uint32_t)(y + row) * scale + sy2) * stride +
                           (uint32_t)(x + col) * scale + sx2] = color;
            }
        }
    }
}

void osd_render(uint64_t frame)
{
    (void)frame; /* presentation count is not a machine-speed metric */
    if (!framebuffer || fb_width < 100 || fb_height < 20)
        return;

    uint32_t scale = fb_width / 640u;
    if (scale < 1u) scale = 1u;
    if (scale > 3u) scale = 3u;
    uint32_t cell = 8u * scale;

    /* MFR identifies the emulated frame. RT is the canonical speed metric:
     * chipset CCK advanced per wall interval as a percentage of realtime.
     * Presentation rate is deliberately not shown as machine speed. */
    char buf[40];
    int  pos = 0;
    buf[pos++] = 'M'; buf[pos++] = 'F'; buf[pos++] = 'R'; buf[pos++] = ':';
    pos += osd_u64_to_str(buf + pos, (int)sizeof(buf) - pos,
                          osd_machine_frame, 8);
    buf[pos++] = ' ';
    buf[pos++] = 'R'; buf[pos++] = 'T'; buf[pos++] = ':';
    pos += osd_u64_to_str(buf + pos, (int)sizeof(buf) - pos,
                          (uint64_t)osd_realtime_percent, 3);
    buf[pos++] = '%';

    /* Live undervoltage/throttle warning: bit0 UV now, bit1 freq capped
     * now, bit2 throttled now. Sticky bits (16-18) don't trigger it. */
    int alert_from = pos;
    if (osd_throttled & 0x7u) {
        buf[pos++] = ' ';
        buf[pos++] = 'U'; buf[pos++] = 'V'; buf[pos++] = '!';
    }
    buf[pos] = '\0';

    int sx     = 4;
    int sy     = 4;
    int text_w = pos * (int)cell;
    int pad    = 2;

    uint32_t stride = pitch / 2u;  /* pitch is bytes; each pixel = 2 bytes */

    /* black background rectangle */
    for (int row = sy - pad; row < sy + (int)cell + pad; row++) {
        uint16_t *line = framebuffer + (uint32_t)row * stride + (uint32_t)(sx - pad);
        for (int col = 0; col < text_w + 2 * pad; col++)
            line[col] = LE16(0x0000u);
    }

    /* white text; the UV! suffix in red (RGB565) */
    uint16_t white = LE16(0xFFFFu);
    uint16_t red   = LE16(0xF800u);
    for (int i = 0; i < pos; i++)
        osd_putchar(framebuffer, stride, sx / (int)scale + i * 8,
                    sy / (int)scale, buf[i],
                    i >= alert_from ? red : white, scale);
}

#endif /* BELLATRIX_OSD */
