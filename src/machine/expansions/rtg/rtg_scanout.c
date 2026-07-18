#include "machine/expansions/rtg/rtg_scanout.h"

#include <string.h>

void bellatrix_rtg_scanout_init(BellatrixRtgScanout *s,
                                uint8_t *vram, uint32_t vram_size,
                                uint32_t vram_offset,
                                uint8_t *frame, size_t frame_capacity)
{
    memset(s, 0, sizeof(*s));
    s->vram = vram;
    s->vram_size = vram_size;
    s->vram_offset = vram_offset;
    s->frame = frame;
    s->frame_capacity = frame_capacity;
}

uint32_t bellatrix_rtg_scanout_reg_read(const BellatrixRtgScanout *s,
                                        uint32_t reg)
{
    switch (reg) {
        case RTG_REG_ID:            return RTG_ID_MAGIC;
        case RTG_REG_VERSION:       return RTG_SPEC_VERSION;
        case RTG_REG_VRAM_OFF:      return s->vram_offset;
        case RTG_REG_VRAM_SIZE:     return s->vram_size;
        case RTG_REG_ENABLE:        return s->enable;
        case RTG_REG_MODE_W:        return s->mode_w;
        case RTG_REG_MODE_H:        return s->mode_h;
        case RTG_REG_FORMAT:        return s->format;
        case RTG_REG_BYTES_PER_ROW: return s->bytes_per_row;
        case RTG_REG_PAN:           return s->pan;
        case RTG_REG_PAL_INDEX:     return s->pal_index;
        case RTG_REG_VBLANK:        return s->vblank;
        default:                    return 0u;
    }
}

void bellatrix_rtg_scanout_reg_write(BellatrixRtgScanout *s,
                                     uint32_t reg, uint32_t value)
{
    switch (reg) {
        case RTG_REG_ENABLE:        s->enable = value & 1u; break;
        case RTG_REG_MODE_W:        s->mode_w = value; break;
        case RTG_REG_MODE_H:        s->mode_h = value; break;
        case RTG_REG_FORMAT:        s->format = value; break;
        case RTG_REG_BYTES_PER_ROW: s->bytes_per_row = value; break;
        case RTG_REG_PAN:           s->pan = value; break;
        case RTG_REG_PAL_INDEX:     s->pal_index = value & 0xffu; break;
        case RTG_REG_PAL_DATA:
            s->palette[s->pal_index] = value & 0x00ffffffu;
            s->pal_index = (s->pal_index + 1u) & 0xffu;
            break;
        default: break;
    }
}

static uint32_t format_bytes(uint32_t format)
{
    switch (format) {
        case RTG_FMT_CLUT:     return 1u;
        case RTG_FMT_R5G6B5:   return 2u;
        case RTG_FMT_A8R8G8B8: return 4u;
        default:               return 0u;
    }
}

int bellatrix_rtg_scanout_active(const BellatrixRtgScanout *s)
{
    uint32_t pixel_bytes = format_bytes(s->format);
    uint64_t row_bytes = (uint64_t)s->mode_w * pixel_bytes;

    return s->vram && s->frame && s->enable && pixel_bytes != 0u &&
           s->mode_w >= 64u && s->mode_w <= 1920u &&
           s->mode_h >= 64u && s->mode_h <= 1080u &&
           s->bytes_per_row >= row_bytes;
}

void bellatrix_rtg_scanout_frame_tick(BellatrixRtgScanout *s)
{
    s->vblank++;
}

int bellatrix_rtg_scanout_render(BellatrixRtgScanout *s,
                                 BellatrixRtgFrame *out)
{
    uint32_t w, h, bpr, x, y, pixel_bytes;
    uint64_t required;
    const uint8_t *src;
    uint8_t *dst;

    if (!out || !bellatrix_rtg_scanout_active(s) || s->pan >= s->vram_size)
        return 0;

    w = s->mode_w;
    h = s->mode_h;
    bpr = s->bytes_per_row;
    pixel_bytes = format_bytes(s->format);
    required = (uint64_t)(h - 1u) * bpr + (uint64_t)w * pixel_bytes;
    if (required > (uint64_t)s->vram_size - s->pan ||
        (uint64_t)w * h * 4u > s->frame_capacity)
        return 0;

    dst = s->frame;
    for (y = 0; y < h; y++) {
        src = s->vram + s->pan + (size_t)y * bpr;
        switch (s->format) {
            case RTG_FMT_CLUT:
                for (x = 0; x < w; x++) {
                    uint32_t rgb = s->palette[src[x]];
                    *dst++ = (uint8_t)(rgb >> 16);
                    *dst++ = (uint8_t)(rgb >> 8);
                    *dst++ = (uint8_t)rgb;
                    *dst++ = 0xffu;
                }
                break;
            case RTG_FMT_R5G6B5:
                for (x = 0; x < w; x++) {
                    uint16_t p = (uint16_t)((src[x * 2u] << 8) |
                                             src[x * 2u + 1u]);
                    *dst++ = (uint8_t)(((p >> 11) & 0x1fu) << 3);
                    *dst++ = (uint8_t)(((p >> 5) & 0x3fu) << 2);
                    *dst++ = (uint8_t)((p & 0x1fu) << 3);
                    *dst++ = 0xffu;
                }
                break;
            case RTG_FMT_A8R8G8B8:
                for (x = 0; x < w; x++) {
                    *dst++ = src[x * 4u + 1u];
                    *dst++ = src[x * 4u + 2u];
                    *dst++ = src[x * 4u + 3u];
                    *dst++ = 0xffu;
                }
                break;
            default:
                return 0;
        }
    }

    out->pixels = s->frame;
    out->width = w;
    out->height = h;
    out->pitch = w * 4u;
    return 1;
}
