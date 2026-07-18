#ifndef BELLATRIX_EXPANSIONS_RTG_SCANOUT_H
#define BELLATRIX_EXPANSIONS_RTG_SCANOUT_H

#include <stddef.h>
#include <stdint.h>

/* Portable register contract shared by the guest card and every presenter. */
#define RTG_REG_ID            0x00u
#define RTG_REG_VERSION       0x04u
#define RTG_REG_VRAM_OFF      0x08u
#define RTG_REG_VRAM_SIZE     0x0Cu
#define RTG_REG_ENABLE        0x10u
#define RTG_REG_MODE_W        0x14u
#define RTG_REG_MODE_H        0x18u
#define RTG_REG_FORMAT        0x1Cu
#define RTG_REG_BYTES_PER_ROW 0x20u
#define RTG_REG_PAN           0x24u
#define RTG_REG_PAL_INDEX     0x28u
#define RTG_REG_PAL_DATA      0x2Cu
#define RTG_REG_VBLANK        0x30u
#define RTG_REG_DEBUG         0x34u
#define RTG_REG_ACCEL_DST     0x38u
#define RTG_REG_ACCEL_PITCH   0x3Cu
#define RTG_REG_ACCEL_XY      0x40u
#define RTG_REG_ACCEL_WH      0x44u
#define RTG_REG_ACCEL_COLOR   0x48u
#define RTG_REG_ACCEL_FMTMASK 0x4Cu
#define RTG_REG_ACCEL_COMMAND 0x50u
#define RTG_REG_ACCEL_STATUS  0x54u
#define RTG_REG_ACCEL_SRC     0x58u
#define RTG_REG_ACCEL_SRC_PITCH 0x5Cu
#define RTG_REG_ACCEL_SRC_XY  0x60u
#define RTG_REG_ACCEL_OPCODE  0x64u
#define RTG_REG_ACCEL_UPLOAD_RESET 0x68u
#define RTG_REG_ACCEL_UPLOAD_DATA  0x6Cu
#define RTG_REG_ACCEL_MODE    0x70u
#define RTG_REG_ACCEL_FGPEN   0x74u
#define RTG_REG_ACCEL_BGPEN   0x78u

#define RTG_ID_MAGIC       0x42525447u /* 'BRTG' */
#define RTG_SPEC_VERSION   1u

#define RTG_FMT_CLUT       1u
#define RTG_FMT_A8R8G8B8   6u
#define RTG_FMT_R5G6B5     10u

#define RTG_ACCEL_FILLRECT 1u
#define RTG_ACCEL_BLIT_COPY 2u
#define RTG_ACCEL_INVERTRECT 3u
#define RTG_ACCEL_BLITTEMPLATE 4u

typedef struct BellatrixRtgFrame {
    const uint8_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
} BellatrixRtgFrame;

typedef struct BellatrixRtgScanout {
    uint8_t *vram;
    uint32_t vram_size;
    uint32_t vram_offset;
    uint8_t *frame;
    size_t frame_capacity;
    uint32_t palette[256];
    uint32_t enable;
    uint32_t mode_w;
    uint32_t mode_h;
    uint32_t format;
    uint32_t bytes_per_row;
    uint32_t pan;
    uint32_t pal_index;
    uint32_t vblank;
} BellatrixRtgScanout;

void bellatrix_rtg_scanout_init(BellatrixRtgScanout *s,
                                uint8_t *vram, uint32_t vram_size,
                                uint32_t vram_offset,
                                uint8_t *frame, size_t frame_capacity);
uint32_t bellatrix_rtg_scanout_reg_read(const BellatrixRtgScanout *s,
                                        uint32_t reg);
void bellatrix_rtg_scanout_reg_write(BellatrixRtgScanout *s,
                                     uint32_t reg, uint32_t value);
int bellatrix_rtg_scanout_active(const BellatrixRtgScanout *s);
void bellatrix_rtg_scanout_frame_tick(BellatrixRtgScanout *s);
int bellatrix_rtg_scanout_render(BellatrixRtgScanout *s,
                                 BellatrixRtgFrame *out);

/* Synchronous accelerator primitive. Returns 1 only when the complete
 * operation was validated and executed; 0 means the guest must fall back. */
int bellatrix_rtg_accel_fillrect(uint8_t *vram, uint32_t vram_size,
                                 uint32_t dst, uint32_t pitch,
                                 uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height,
                                 uint32_t color, uint32_t format,
                                 uint32_t mask);
int bellatrix_rtg_accel_blit_copy(uint8_t *vram, uint32_t vram_size,
                                  uint32_t src, uint32_t src_pitch,
                                  uint32_t src_x, uint32_t src_y,
                                  uint32_t dst, uint32_t dst_pitch,
                                  uint32_t dst_x, uint32_t dst_y,
                                  uint32_t width, uint32_t height,
                                  uint32_t format);
int bellatrix_rtg_accel_invertrect(uint8_t *vram, uint32_t vram_size,
                                   uint32_t dst, uint32_t pitch,
                                   uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height,
                                   uint32_t format, uint32_t mask);
int bellatrix_rtg_accel_blittemplate(uint8_t *vram, uint32_t vram_size,
                                     uint32_t dst, uint32_t pitch,
                                     uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height,
                                     uint32_t format, uint32_t mask,
                                     const uint8_t *bits, uint32_t bits_size,
                                     uint32_t bits_pitch, uint32_t xoffset,
                                     uint32_t drawmode, uint32_t fg,
                                     uint32_t bg);

#endif
