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

int main(void)
{
    test_register_contract();
    test_clut_stride_and_pan();
    test_rgb565();
    test_argb_and_bounds();
    test_accel_fillrect();
    test_accel_blit_copy();
    puts("rtg scanout tests passed");
    return 0;
}
