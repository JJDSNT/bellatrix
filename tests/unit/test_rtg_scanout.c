#include "machine/expansions/rtg/rtg_scanout.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u

static uint8_t s_vram[65536];
static uint8_t s_frame[W * H * 4u];

static void fail_u32(const char *name, uint32_t expected, uint32_t actual)
{
    fprintf(stderr, "FAIL %s expected=%08x actual=%08x\n",
            name, (unsigned)expected, (unsigned)actual);
    exit(1);
}

static void check_u32(const char *name, uint32_t expected, uint32_t actual)
{
    if (expected != actual)
        fail_u32(name, expected, actual);
}

static void check_pixel(const char *name, const uint8_t *p,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
        fprintf(stderr,
                "FAIL %s expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x\n",
                name, r, g, b, a, p[0], p[1], p[2], p[3]);
        exit(1);
    }
}

static void configure(BellatrixRtgScanout *s, uint32_t format,
                      uint32_t stride, uint32_t pan)
{
    bellatrix_rtg_scanout_reg_write(s, RTG_REG_MODE_W, W);
    bellatrix_rtg_scanout_reg_write(s, RTG_REG_MODE_H, H);
    bellatrix_rtg_scanout_reg_write(s, RTG_REG_FORMAT, format);
    bellatrix_rtg_scanout_reg_write(s, RTG_REG_BYTES_PER_ROW, stride);
    bellatrix_rtg_scanout_reg_write(s, RTG_REG_PAN, pan);
    bellatrix_rtg_scanout_reg_write(s, RTG_REG_ENABLE, 1u);
}

static void test_register_contract(void)
{
    BellatrixRtgScanout s;

    bellatrix_rtg_scanout_init(&s, s_vram, sizeof(s_vram), 0x3000u,
                               s_frame, sizeof(s_frame));
    check_u32("id", RTG_ID_MAGIC,
              bellatrix_rtg_scanout_reg_read(&s, RTG_REG_ID));
    check_u32("version", RTG_SPEC_VERSION,
              bellatrix_rtg_scanout_reg_read(&s, RTG_REG_VERSION));
    check_u32("vram offset", 0x3000u,
              bellatrix_rtg_scanout_reg_read(&s, RTG_REG_VRAM_OFF));
    check_u32("vram size", sizeof(s_vram),
              bellatrix_rtg_scanout_reg_read(&s, RTG_REG_VRAM_SIZE));

    bellatrix_rtg_scanout_reg_write(&s, RTG_REG_PAL_INDEX, 255u);
    bellatrix_rtg_scanout_reg_write(&s, RTG_REG_PAL_DATA, 0xff123456u);
    check_u32("palette masks alpha", 0x00123456u, s.palette[255]);
    check_u32("palette wraps", 0u,
              bellatrix_rtg_scanout_reg_read(&s, RTG_REG_PAL_INDEX));

    bellatrix_rtg_scanout_frame_tick(&s);
    check_u32("vblank tick", 1u,
              bellatrix_rtg_scanout_reg_read(&s, RTG_REG_VBLANK));
}

static void test_clut_stride_and_pan(void)
{
    BellatrixRtgScanout s;
    BellatrixRtgFrame out;
    const uint32_t pan = 5u;
    const uint32_t stride = W + 7u;

    memset(s_vram, 0, sizeof(s_vram));
    bellatrix_rtg_scanout_init(&s, s_vram, sizeof(s_vram), 0u,
                               s_frame, sizeof(s_frame));
    bellatrix_rtg_scanout_reg_write(&s, RTG_REG_PAL_INDEX, 1u);
    bellatrix_rtg_scanout_reg_write(&s, RTG_REG_PAL_DATA, 0x00112233u);
    bellatrix_rtg_scanout_reg_write(&s, RTG_REG_PAL_DATA, 0x00abcdefu);
    configure(&s, RTG_FMT_CLUT, stride, pan);
    s_vram[pan] = 1u;
    s_vram[pan + (H - 1u) * stride + (W - 1u)] = 2u;

    check_u32("clut active", 1u, bellatrix_rtg_scanout_active(&s));
    check_u32("clut render", 1u, bellatrix_rtg_scanout_render(&s, &out));
    check_u32("clut width", W, out.width);
    check_u32("clut height", H, out.height);
    check_u32("clut pitch", W * 4u, out.pitch);
    check_pixel("clut first", out.pixels, 0x11u, 0x22u, 0x33u, 0xffu);
    check_pixel("clut last", out.pixels + (W * H - 1u) * 4u,
                0xabu, 0xcdu, 0xefu, 0xffu);
}

static void test_rgb565(void)
{
    BellatrixRtgScanout s;
    BellatrixRtgFrame out;
    const uint32_t stride = W * 2u + 4u;

    memset(s_vram, 0, sizeof(s_vram));
    bellatrix_rtg_scanout_init(&s, s_vram, sizeof(s_vram), 0u,
                               s_frame, sizeof(s_frame));
    configure(&s, RTG_FMT_R5G6B5, stride, 3u);
    s_vram[3] = 0xf8u; s_vram[4] = 0x00u;
    s_vram[3u + stride] = 0x07u; s_vram[4u + stride] = 0xe0u;

    check_u32("565 render", 1u, bellatrix_rtg_scanout_render(&s, &out));
    check_pixel("565 red", out.pixels, 0xf8u, 0x00u, 0x00u, 0xffu);
    check_pixel("565 green next row", out.pixels + W * 4u,
                0x00u, 0xfcu, 0x00u, 0xffu);
}

static void test_argb_and_bounds(void)
{
    BellatrixRtgScanout s;
    BellatrixRtgFrame out;
    const uint32_t stride = W * 4u;

    memset(s_vram, 0, sizeof(s_vram));
    bellatrix_rtg_scanout_init(&s, s_vram, sizeof(s_vram), 0u,
                               s_frame, sizeof(s_frame));
    configure(&s, RTG_FMT_A8R8G8B8, stride, 0u);
    s_vram[0] = 0x99u; s_vram[1] = 0x12u;
    s_vram[2] = 0x34u; s_vram[3] = 0x56u;
    check_u32("argb render", 1u, bellatrix_rtg_scanout_render(&s, &out));
    check_pixel("argb ignores guest alpha", out.pixels,
                0x12u, 0x34u, 0x56u, 0xffu);

    bellatrix_rtg_scanout_reg_write(&s, RTG_REG_PAN, sizeof(s_vram));
    check_u32("pan outside vram rejected", 0u,
              bellatrix_rtg_scanout_render(&s, &out));
    bellatrix_rtg_scanout_reg_write(&s, RTG_REG_PAN,
                                    sizeof(s_vram) - stride);
    check_u32("truncated image rejected", 0u,
              bellatrix_rtg_scanout_render(&s, &out));
    bellatrix_rtg_scanout_reg_write(&s, RTG_REG_PAN, 0u);
    bellatrix_rtg_scanout_reg_write(&s, RTG_REG_BYTES_PER_ROW, stride - 1u);
    check_u32("short stride inactive", 0u, bellatrix_rtg_scanout_active(&s));
}

static void test_accel_fillrect(void)
{
    uint8_t vram[64];

    memset(vram, 0x11, sizeof(vram));
    check_u32("fillrect handled", 1u,
              bellatrix_rtg_accel_fillrect(vram, sizeof(vram),
                                           4u, 8u, 2u, 1u, 3u, 2u,
                                           0x5au, RTG_FMT_CLUT, 0xffu));
    check_u32("fillrect row0 first", 0x5au, vram[14]);
    check_u32("fillrect row0 last", 0x5au, vram[16]);
    check_u32("fillrect row1 first", 0x5au, vram[22]);
    check_u32("fillrect neighbor intact", 0x11u, vram[13]);
    check_u32("fillrect rejects mask", 0u,
              bellatrix_rtg_accel_fillrect(vram, sizeof(vram),
                                           0u, 8u, 0u, 0u, 2u, 2u,
                                           0u, RTG_FMT_CLUT, 0x0fu));
    check_u32("fillrect rgb565 handled", 1u,
              bellatrix_rtg_accel_fillrect(vram, sizeof(vram),
                                           0u, 8u, 1u, 0u, 2u, 1u,
                                           0x1234u, RTG_FMT_R5G6B5, 0xffu));
    check_u32("fillrect rgb565 byte0", 0x12u, vram[2]);
    check_u32("fillrect rgb565 byte1", 0x34u, vram[3]);
    check_u32("fillrect argb handled", 1u,
              bellatrix_rtg_accel_fillrect(vram, sizeof(vram),
                                           0u, 8u, 1u, 1u, 1u, 1u,
                                           0x11223344u,
                                           RTG_FMT_A8R8G8B8, 0xffu));
    check_u32("fillrect argb alpha", 0x11u, vram[12]);
    check_u32("fillrect argb blue", 0x44u, vram[15]);
    check_u32("fillrect rejects row overflow", 0u,
              bellatrix_rtg_accel_fillrect(vram, sizeof(vram),
                                           0u, 8u, 7u, 0u, 2u, 1u,
                                           0u, RTG_FMT_CLUT, 0xffu));
    check_u32("fillrect rejects vram overflow", 0u,
              bellatrix_rtg_accel_fillrect(vram, sizeof(vram),
                                           60u, 8u, 0u, 0u, 8u, 1u,
                                           0u, RTG_FMT_CLUT, 0xffu));
}

static void test_accel_blit_copy(void)
{
    uint8_t vram[64];
    uint32_t i;

    for (i = 0u; i < sizeof(vram); ++i)
        vram[i] = (uint8_t)i;
    check_u32("blit copy handled", 1u,
              bellatrix_rtg_accel_blit_copy(vram, sizeof(vram),
                                             0u, 8u, 1u, 0u,
                                             0u, 8u, 2u, 2u,
                                             3u, 2u, RTG_FMT_CLUT));
    check_u32("blit copy row0", 1u, vram[18]);
    check_u32("blit copy row1", 9u, vram[26]);

    for (i = 0u; i < sizeof(vram); ++i)
        vram[i] = (uint8_t)i;
    check_u32("blit overlap down", 1u,
              bellatrix_rtg_accel_blit_copy(vram, sizeof(vram),
                                             0u, 8u, 0u, 0u,
                                             0u, 8u, 0u, 1u,
                                             8u, 3u, RTG_FMT_CLUT));
    check_u32("blit overlap preserved", 0u, vram[8]);
    check_u32("blit overlap last row", 16u, vram[24]);
    check_u32("blit rgb565 handled", 1u,
              bellatrix_rtg_accel_blit_copy(vram, sizeof(vram),
                                             0u, 8u, 0u, 0u,
                                             32u, 8u, 0u, 0u,
                                             2u, 1u, RTG_FMT_R5G6B5));
    check_u32("blit rejects row overflow", 0u,
              bellatrix_rtg_accel_blit_copy(vram, sizeof(vram),
                                             0u, 8u, 3u, 0u,
                                             0u, 8u, 0u, 0u,
                                             2u, 1u, RTG_FMT_R5G6B5));
}

static void test_accel_invertrect(void)
{
    uint8_t vram[32];
    memset(vram, 0x55, sizeof(vram));
    check_u32("invert handled", 1u,
              bellatrix_rtg_accel_invertrect(vram, sizeof(vram),
                                              0u, 8u, 1u, 1u, 2u, 1u,
                                              RTG_FMT_R5G6B5, 0xffu));
    check_u32("invert first", 0xaau, vram[10]);
    check_u32("invert last", 0xaau, vram[13]);
    check_u32("invert neighbor", 0x55u, vram[9]);
    check_u32("invert rejects mask", 0u,
              bellatrix_rtg_accel_invertrect(vram, sizeof(vram),
                                              0u, 8u, 0u, 0u, 1u, 1u,
                                              RTG_FMT_CLUT, 0x0fu));
}

static void test_accel_blittemplate(void)
{
    uint8_t vram[64], bits[2] = { 0xa0u, 0x40u };
    memset(vram, 0x11, sizeof(vram));
    check_u32("template jam1", 1u,
              bellatrix_rtg_accel_blittemplate(
                  vram, sizeof(vram), 0u, 8u, 1u, 1u, 3u, 2u,
                  RTG_FMT_CLUT, 0xffu, bits, sizeof(bits), 1u, 0u,
                  0u, 0x77u, 0x22u));
    check_u32("template fg row0 col0", 0x77u, vram[9]);
    check_u32("template transparent", 0x11u, vram[10]);
    check_u32("template fg row0 col2", 0x77u, vram[11]);
    check_u32("template fg row1 col1", 0x77u, vram[18]);
    check_u32("template jam2 inverse", 1u,
              bellatrix_rtg_accel_blittemplate(
                  vram, sizeof(vram), 0u, 8u, 0u, 0u, 3u, 1u,
                  RTG_FMT_CLUT, 0xffu, bits, sizeof(bits), 1u, 0u,
                  5u, 0x33u, 0x44u));
    check_u32("template inverse bg", 0x44u, vram[0]);
    check_u32("template inverse fg", 0x33u, vram[1]);
    check_u32("template rejects short upload", 0u,
              bellatrix_rtg_accel_blittemplate(
                  vram, sizeof(vram), 0u, 8u, 0u, 0u, 8u, 2u,
                  RTG_FMT_CLUT, 0xffu, bits, 1u, 1u, 0u,
                  0u, 1u, 0u));
}

static void test_accel_blitpattern(void)
{
    uint8_t vram[64], bits[4] = { 0xa0u, 0x00u, 0x40u, 0x00u };
    memset(vram, 0x11, sizeof(vram));
    check_u32("pattern jam2", 1u,
              bellatrix_rtg_accel_blitpattern(
                  vram, sizeof(vram), 0u, 8u, 0u, 0u, 4u, 3u,
                  RTG_FMT_CLUT, 0xffu, bits, sizeof(bits), 2u,
                  0u, 0u, 1u, 0x77u, 0x22u));
    check_u32("pattern row0 fg", 0x77u, vram[0]);
    check_u32("pattern row0 bg", 0x22u, vram[1]);
    check_u32("pattern row1 fg", 0x77u, vram[9]);
    check_u32("pattern repeats", 0x77u, vram[16]);
    check_u32("pattern rejects height", 0u,
              bellatrix_rtg_accel_blitpattern(
                  vram, sizeof(vram), 0u, 8u, 0u, 0u, 1u, 1u,
                  RTG_FMT_CLUT, 0xffu, bits, sizeof(bits), 3u,
                  0u, 0u, 0u, 1u, 0u));
}

static void test_accel_drawline(void)
{
    uint8_t vram[64];
    memset(vram, 0x11, sizeof(vram));
    check_u32("line diagonal", 1u,
              bellatrix_rtg_accel_drawline(vram, sizeof(vram),
                                            0u, 8u, 1u, 1u, 4u, 4u,
                                            0x77u, RTG_FMT_CLUT));
    check_u32("line start", 0x77u, vram[9]);
    check_u32("line middle", 0x77u, vram[27]);
    check_u32("line end", 0x77u, vram[36]);
    check_u32("line neighbor", 0x11u, vram[10]);
    check_u32("line rgb565", 1u,
              bellatrix_rtg_accel_drawline(vram, sizeof(vram),
                                            0u, 8u, 0u, 0u, 2u, 0u,
                                            0x1234u, RTG_FMT_R5G6B5));
    check_u32("line rgb565 hi", 0x12u, vram[4]);
    check_u32("line rgb565 lo", 0x34u, vram[5]);
    check_u32("line rejects pitch", 0u,
              bellatrix_rtg_accel_drawline(vram, sizeof(vram),
                                            0u, 8u, 0u, 0u, 4u, 0u,
                                            0u, RTG_FMT_R5G6B5));
}

static void test_accel_planar2chunky(void)
{
    uint8_t vram[32], planes[2] = { 0xa0u, 0xc0u };
    memset(vram, 0x80, sizeof(vram));
    check_u32("planar chunky", 1u,
              bellatrix_rtg_accel_planar2chunky(
                  vram, sizeof(vram), 0u, 8u, 1u, 1u, 4u, 1u,
                  planes, sizeof(planes), 1u, 0u, 2u, 0x03u));
    check_u32("planar pixel 3", 0x83u, vram[9]);
    check_u32("planar pixel 2", 0x82u, vram[10]);
    check_u32("planar pixel 1", 0x81u, vram[11]);
    check_u32("planar pixel 0", 0x80u, vram[12]);
    check_u32("planar neighbor", 0x80u, vram[8]);
    check_u32("planar rejects depth", 0u,
              bellatrix_rtg_accel_planar2chunky(
                  vram, sizeof(vram), 0u, 8u, 0u, 0u, 1u, 1u,
                  planes, sizeof(planes), 1u, 0u, 9u, 0xffu));
}

static void test_accel_planar2direct(void)
{
    uint8_t vram[64];
    uint8_t upload565[18] = {
        0xa0u, 0xc0u,
        0,0,0,0, 0,0,0x12,0x34, 0,0,0x56,0x78, 0,0,0x9a,0xbc
    };
    uint8_t upload32[9] = {
        0x80u, 0,0,0,0, 0,0x11,0x22,0x33
    };
    memset(vram, 0, sizeof(vram));
    check_u32("planar direct 565", 1u,
              bellatrix_rtg_accel_planar2direct(
                  vram, sizeof(vram), 0u, 8u, 0u, 0u, 4u, 1u,
                  RTG_FMT_R5G6B5, upload565, sizeof(upload565),
                  1u, 0u, 2u, 0x03u, 0xffffu));
    check_u32("direct 565 p3 hi", 0x9au, vram[0]);
    check_u32("direct 565 p3 lo", 0xbcu, vram[1]);
    check_u32("direct 565 p2", 0x56u, vram[2]);
    vram[16] = 0xaau; vram[17] = 0x55u; vram[18] = 0x66u; vram[19] = 0x77u;
    check_u32("planar direct argb", 1u,
              bellatrix_rtg_accel_planar2direct(
                  vram, sizeof(vram), 0u, 8u, 0u, 2u, 1u, 1u,
                  RTG_FMT_A8R8G8B8, upload32, sizeof(upload32),
                  1u, 0u, 1u, 0x01u, 0x00ffffffu));
    check_u32("direct preserves alpha", 0xaau, vram[16]);
    check_u32("direct argb red", 0x11u, vram[17]);
    check_u32("direct argb blue", 0x33u, vram[19]);
    check_u32("direct rejects clut", 0u,
              bellatrix_rtg_accel_planar2direct(
                  vram, sizeof(vram), 0u, 8u, 0u, 0u, 1u, 1u,
                  RTG_FMT_CLUT, upload32, sizeof(upload32),
                  1u, 0u, 1u, 1u, 0xffu));
}

int main(void)
{
    test_register_contract();
    test_clut_stride_and_pan();
    test_rgb565();
    test_argb_and_bounds();
    test_accel_fillrect();
    test_accel_blit_copy();
    test_accel_invertrect();
    test_accel_blittemplate();
    test_accel_blitpattern();
    test_accel_drawline();
    test_accel_planar2chunky();
    test_accel_planar2direct();
    puts("rtg scanout tests passed");
    return 0;
}
