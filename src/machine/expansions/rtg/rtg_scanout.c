#include "machine/expansions/rtg/rtg_scanout.h"

#include <string.h>

static uint32_t format_bytes(uint32_t format);

static void update_sprite_rgba(BellatrixRtgScanout *s)
{
    uint32_t i, count = (uint32_t)s->sprite_w * s->sprite_h;

    if (!s->sprite_image_dirty)
        return;
    for (i = 0u; i < count; ++i) {
        uint32_t color = s->sprite_colors[s->sprite_indices[i] & 3u];
        uint8_t *dst = s->sprite_rgba + (size_t)i * 4u;
        dst[0] = (uint8_t)(color >> 16);
        dst[1] = (uint8_t)(color >> 8);
        dst[2] = (uint8_t)color;
        dst[3] = s->sprite_indices[i] ? 0xffu : 0u;
    }
    s->sprite_image_dirty = 0u;
}

static uint32_t hash_tile(const uint8_t *base, uint32_t pitch,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t pixel_bytes)
{
    uint32_t hash = 2166136261u;
    uint32_t row, i;
    size_t row_bytes = (size_t)w * pixel_bytes;

    for (row = 0u; row < h; ++row) {
        const uint8_t *p = base + (size_t)(y + row) * pitch +
                           (size_t)x * pixel_bytes;
        for (i = 0u; i < row_bytes; ++i) {
            hash ^= p[i];
            hash *= 16777619u;
        }
    }
    return hash;
}

static void convert_rect(BellatrixRtgScanout *s, uint32_t x0, uint32_t y0,
                         uint32_t width, uint32_t height)
{
    uint32_t x, y;

    for (y = y0; y < y0 + height; ++y) {
        const uint8_t *src = s->vram + s->pan + (size_t)y * s->bytes_per_row;
        uint8_t *dst = s->frame + ((size_t)y * s->mode_w + x0) * 4u;
        switch (s->format) {
        case RTG_FMT_CLUT:
            for (x = x0; x < x0 + width; ++x) {
                uint32_t rgb = s->palette[src[x]];
                *dst++ = (uint8_t)(rgb >> 16); *dst++ = (uint8_t)(rgb >> 8);
                *dst++ = (uint8_t)rgb; *dst++ = 0xffu;
            }
            break;
        case RTG_FMT_R5G6B5:
            for (x = x0; x < x0 + width; ++x) {
                uint16_t p = (uint16_t)((src[x * 2u] << 8) | src[x * 2u + 1u]);
                *dst++ = (uint8_t)(((p >> 11) & 0x1fu) << 3);
                *dst++ = (uint8_t)(((p >> 5) & 0x3fu) << 2);
                *dst++ = (uint8_t)((p & 0x1fu) << 3); *dst++ = 0xffu;
            }
            break;
        case RTG_FMT_A8R8G8B8:
            for (x = x0; x < x0 + width; ++x) {
                *dst++ = src[x * 4u + 1u]; *dst++ = src[x * 4u + 2u];
                *dst++ = src[x * 4u + 3u]; *dst++ = 0xffu;
            }
            break;
        }
    }
}

int bellatrix_rtg_accel_fillrect(uint8_t *vram, uint32_t vram_size,
                                 uint32_t dst, uint32_t pitch,
                                 uint32_t x, uint32_t y,
                                 uint32_t width, uint32_t height,
                                 uint32_t color, uint32_t format,
                                 uint32_t mask)
{
    uint64_t first, last, row_bytes;
    uint32_t row, col, pixel_bytes;

    pixel_bytes = format_bytes(format);
    if (!vram || pixel_bytes == 0u || mask != 0xffu ||
        width == 0u || height == 0u || pitch == 0u ||
        (uint64_t)x * pixel_bytes > pitch)
        return 0;
    row_bytes = (uint64_t)width * pixel_bytes;
    if (row_bytes > pitch - (uint64_t)x * pixel_bytes)
        return 0;
    first = (uint64_t)dst + (uint64_t)y * pitch +
            (uint64_t)x * pixel_bytes;
    last = first + (uint64_t)(height - 1u) * pitch + row_bytes;
    if (first >= vram_size || last > vram_size)
        return 0;
    for (row = 0u; row < height; ++row) {
        uint8_t *out = vram + (size_t)(first + (uint64_t)row * pitch);
        if (pixel_bytes == 1u) {
            memset(out, (int)(uint8_t)color, width);
        } else {
            for (col = 0u; col < width; ++col) {
                if (pixel_bytes == 2u) {
                    out[0] = (uint8_t)(color >> 8);
                    out[1] = (uint8_t)color;
                } else {
                    out[0] = (uint8_t)(color >> 24);
                    out[1] = (uint8_t)(color >> 16);
                    out[2] = (uint8_t)(color >> 8);
                    out[3] = (uint8_t)color;
                }
                out += pixel_bytes;
            }
        }
    }
    return 1;
}

int bellatrix_rtg_accel_blit_copy(uint8_t *vram, uint32_t vram_size,
                                  uint32_t src, uint32_t src_pitch,
                                  uint32_t src_x, uint32_t src_y,
                                  uint32_t dst, uint32_t dst_pitch,
                                  uint32_t dst_x, uint32_t dst_y,
                                  uint32_t width, uint32_t height,
                                  uint32_t format)
{
    uint32_t pixel_bytes = format_bytes(format), row;
    uint64_t row_bytes, src_first, src_last, dst_first, dst_last;
    int backwards;

    if (!vram || pixel_bytes == 0u || width == 0u || height == 0u ||
        src_pitch == 0u || dst_pitch == 0u)
        return 0;
    row_bytes = (uint64_t)width * pixel_bytes;
    if ((uint64_t)src_x * pixel_bytes > src_pitch ||
        row_bytes > src_pitch - (uint64_t)src_x * pixel_bytes ||
        (uint64_t)dst_x * pixel_bytes > dst_pitch ||
        row_bytes > dst_pitch - (uint64_t)dst_x * pixel_bytes)
        return 0;
    src_first = (uint64_t)src + (uint64_t)src_y * src_pitch +
                (uint64_t)src_x * pixel_bytes;
    dst_first = (uint64_t)dst + (uint64_t)dst_y * dst_pitch +
                (uint64_t)dst_x * pixel_bytes;
    src_last = src_first + (uint64_t)(height - 1u) * src_pitch + row_bytes;
    dst_last = dst_first + (uint64_t)(height - 1u) * dst_pitch + row_bytes;
    if (src_first >= vram_size || dst_first >= vram_size ||
        src_last > vram_size || dst_last > vram_size)
        return 0;
    backwards = dst_first > src_first && dst_first < src_last;
    for (row = 0u; row < height; ++row) {
        uint32_t r = backwards ? height - 1u - row : row;
        memmove(vram + (size_t)(dst_first + (uint64_t)r * dst_pitch),
                vram + (size_t)(src_first + (uint64_t)r * src_pitch),
                (size_t)row_bytes);
    }
    return 1;
}

int bellatrix_rtg_accel_invertrect(uint8_t *vram, uint32_t vram_size,
                                   uint32_t dst, uint32_t pitch,
                                   uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height,
                                   uint32_t format, uint32_t mask)
{
    uint32_t bpp = format_bytes(format), row;
    uint64_t first, last, row_bytes;
    if (!vram || !bpp || mask != 0xffu || !width || !height || !pitch ||
        (uint64_t)x * bpp > pitch)
        return 0;
    row_bytes = (uint64_t)width * bpp;
    if (row_bytes > pitch - (uint64_t)x * bpp)
        return 0;
    first = (uint64_t)dst + (uint64_t)y * pitch + (uint64_t)x * bpp;
    last = first + (uint64_t)(height - 1u) * pitch + row_bytes;
    if (first >= vram_size || last > vram_size)
        return 0;
    for (row = 0; row < height; ++row) {
        uint8_t *p = vram + first + (uint64_t)row * pitch;
        uint64_t i;
        for (i = 0; i < row_bytes; ++i)
            p[i] = (uint8_t)~p[i];
    }
    return 1;
}

static void store_pixel(uint8_t *p, uint32_t bpp, uint32_t color)
{
    if (bpp == 1u) p[0] = (uint8_t)color;
    else if (bpp == 2u) { p[0] = (uint8_t)(color >> 8); p[1] = (uint8_t)color; }
    else { p[0] = (uint8_t)(color >> 24); p[1] = (uint8_t)(color >> 16);
           p[2] = (uint8_t)(color >> 8); p[3] = (uint8_t)color; }
}

int bellatrix_rtg_accel_blittemplate(uint8_t *vram, uint32_t vram_size,
                                     uint32_t dst, uint32_t pitch,
                                     uint32_t x, uint32_t y,
                                     uint32_t width, uint32_t height,
                                     uint32_t format, uint32_t mask,
                                     const uint8_t *bits, uint32_t bits_size,
                                     uint32_t bits_pitch, uint32_t xoffset,
                                     uint32_t drawmode, uint32_t fg,
                                     uint32_t bg)
{
    uint32_t bpp = format_bytes(format), row, col;
    uint64_t first, last, row_bytes, needed;
    if (!vram || !bits || !bpp || mask != 0xffu || !width || !height ||
        !pitch || !bits_pitch || xoffset > 15u || (drawmode & ~5u) != 0u ||
        (drawmode & 1u) > 1u || (uint64_t)x * bpp > pitch)
        return 0;
    row_bytes = (uint64_t)width * bpp;
    if (row_bytes > pitch - (uint64_t)x * bpp ||
        (uint64_t)xoffset + width > (uint64_t)bits_pitch * 8u)
        return 0;
    needed = (uint64_t)(height - 1u) * bits_pitch +
             ((uint64_t)xoffset + width + 7u) / 8u;
    first = (uint64_t)dst + (uint64_t)y * pitch + (uint64_t)x * bpp;
    last = first + (uint64_t)(height - 1u) * pitch + row_bytes;
    if (needed > bits_size || first >= vram_size || last > vram_size)
        return 0;
    for (row = 0; row < height; ++row) {
        uint8_t *out = vram + first + (uint64_t)row * pitch;
        for (col = 0; col < width; ++col) {
            uint32_t bitpos = xoffset + col;
            uint32_t set = (bits[(uint64_t)row * bits_pitch + bitpos / 8u] >>
                            (7u - bitpos % 8u)) & 1u;
            if (drawmode & 4u) set ^= 1u;
            if (set || (drawmode & 1u))
                store_pixel(out, bpp, set ? fg : bg);
            out += bpp;
        }
    }
    return 1;
}

int bellatrix_rtg_accel_blitpattern(uint8_t *vram, uint32_t vram_size,
                                    uint32_t dst, uint32_t pitch,
                                    uint32_t x, uint32_t y,
                                    uint32_t width, uint32_t height,
                                    uint32_t format, uint32_t mask,
                                    const uint8_t *bits, uint32_t bits_size,
                                    uint32_t pattern_height,
                                    uint32_t xoffset, uint32_t yoffset,
                                    uint32_t drawmode, uint32_t fg,
                                    uint32_t bg)
{
    uint32_t bpp = format_bytes(format), row, col;
    uint64_t first, last, row_bytes;
    if (!vram || !bits || !bpp || mask != 0xffu || !width || !height ||
        !pitch || !pattern_height || pattern_height > 256u ||
        (pattern_height & (pattern_height - 1u)) != 0u ||
        bits_size < pattern_height * 2u || xoffset > 15u ||
        (drawmode & ~5u) != 0u || (uint64_t)x * bpp > pitch)
        return 0;
    row_bytes = (uint64_t)width * bpp;
    if (row_bytes > pitch - (uint64_t)x * bpp)
        return 0;
    first = (uint64_t)dst + (uint64_t)y * pitch + (uint64_t)x * bpp;
    last = first + (uint64_t)(height - 1u) * pitch + row_bytes;
    if (first >= vram_size || last > vram_size)
        return 0;
    for (row = 0; row < height; ++row) {
        uint32_t pr = (yoffset + row) & (pattern_height - 1u);
        uint32_t word = ((uint32_t)bits[pr * 2u] << 8) | bits[pr * 2u + 1u];
        uint8_t *out = vram + first + (uint64_t)row * pitch;
        for (col = 0; col < width; ++col) {
            uint32_t set = (word >> (15u - ((xoffset + col) & 15u))) & 1u;
            if (drawmode & 4u) set ^= 1u;
            if (set || (drawmode & 1u)) store_pixel(out, bpp, set ? fg : bg);
            out += bpp;
        }
    }
    return 1;
}

int bellatrix_rtg_accel_drawline(uint8_t *vram, uint32_t vram_size,
                                 uint32_t dst, uint32_t pitch,
                                 uint32_t x0, uint32_t y0,
                                 uint32_t x1, uint32_t y1,
                                 uint32_t color, uint32_t format)
{
    uint32_t bpp = format_bytes(format);
    int64_t x = x0, y = y0, tx = x1, ty = y1;
    int64_t dx, dy, sx, sy, err;
    uint64_t end0, end1;
    if (!vram || !bpp || !pitch ||
        (uint64_t)x0 * bpp + bpp > pitch ||
        (uint64_t)x1 * bpp + bpp > pitch)
        return 0;
    end0 = (uint64_t)dst + (uint64_t)y0 * pitch + (uint64_t)x0 * bpp + bpp;
    end1 = (uint64_t)dst + (uint64_t)y1 * pitch + (uint64_t)x1 * bpp + bpp;
    if (end0 > vram_size || end1 > vram_size)
        return 0;
    dx = tx > x ? tx - x : x - tx;
    sx = x < tx ? 1 : -1;
    dy = ty > y ? y - ty : ty - y;
    sy = y < ty ? 1 : -1;
    err = dx + dy;
    for (;;) {
        store_pixel(vram + dst + (uint64_t)y * pitch + (uint64_t)x * bpp,
                    bpp, color);
        if (x == tx && y == ty) break;
        if (2 * err >= dy) { err += dy; x += sx; }
        if (2 * err <= dx) { err += dx; y += sy; }
    }
    return 1;
}

int bellatrix_rtg_accel_planar2chunky(uint8_t *vram, uint32_t vram_size,
                                      uint32_t dst, uint32_t pitch,
                                      uint32_t dx, uint32_t dy,
                                      uint32_t width, uint32_t height,
                                      const uint8_t *planes,
                                      uint32_t planes_size,
                                      uint32_t plane_pitch,
                                      uint32_t source_bit,
                                      uint32_t depth, uint32_t plane_mask)
{
    uint32_t row, col, plane;
    uint64_t first, last, plane_span, needed;
    if (!vram || !planes || !pitch || !width || !height || !plane_pitch ||
        !depth || depth > 8u || source_bit > 7u || dx > pitch ||
        width > pitch - dx || source_bit + width > plane_pitch * 8u)
        return 0;
    plane_span = (uint64_t)plane_pitch * height;
    needed = plane_span * depth;
    first = (uint64_t)dst + (uint64_t)dy * pitch + dx;
    last = first + (uint64_t)(height - 1u) * pitch + width;
    if (needed > planes_size || first >= vram_size || last > vram_size)
        return 0;
    for (row = 0; row < height; ++row) {
        uint8_t *out = vram + first + (uint64_t)row * pitch;
        for (col = 0; col < width; ++col) {
            uint32_t pixel = out[col];
            uint32_t bitpos = source_bit + col;
            for (plane = 0; plane < depth; ++plane) {
                uint32_t bit;
                if (!(plane_mask & (1u << plane))) continue;
                bit = (planes[(uint64_t)plane * plane_span +
                              (uint64_t)row * plane_pitch + bitpos / 8u] >>
                       (7u - bitpos % 8u)) & 1u;
                pixel = (pixel & ~(1u << plane)) | (bit << plane);
            }
            out[col] = (uint8_t)pixel;
        }
    }
    return 1;
}

static uint32_t load_pixel(const uint8_t *p, uint32_t bpp)
{
    if (bpp == 2u) return ((uint32_t)p[0] << 8) | p[1];
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int bellatrix_rtg_accel_planar2direct(uint8_t *vram, uint32_t vram_size,
                                      uint32_t dst, uint32_t pitch,
                                      uint32_t dx, uint32_t dy,
                                      uint32_t width, uint32_t height,
                                      uint32_t format,
                                      const uint8_t *upload,
                                      uint32_t upload_size,
                                      uint32_t plane_pitch,
                                      uint32_t source_bit,
                                      uint32_t depth, uint32_t plane_mask,
                                      uint32_t color_mask)
{
    uint32_t bpp = format_bytes(format), row, col, plane, colors;
    uint64_t first, last, row_bytes, plane_span, map_offset, needed;
    if (!vram || !upload || (bpp != 2u && bpp != 4u) || !pitch ||
        !width || !height || !plane_pitch || !depth || depth > 8u ||
        source_bit > 7u || (uint64_t)dx * bpp > pitch ||
        source_bit + width > plane_pitch * 8u)
        return 0;
    row_bytes = (uint64_t)width * bpp;
    if (row_bytes > pitch - (uint64_t)dx * bpp)
        return 0;
    colors = 1u << depth;
    plane_span = (uint64_t)plane_pitch * height;
    map_offset = plane_span * depth;
    needed = map_offset + (uint64_t)colors * 4u;
    first = (uint64_t)dst + (uint64_t)dy * pitch + (uint64_t)dx * bpp;
    last = first + (uint64_t)(height - 1u) * pitch + row_bytes;
    if (needed > upload_size || first >= vram_size || last > vram_size)
        return 0;
    for (row = 0; row < height; ++row) {
        uint8_t *out = vram + first + (uint64_t)row * pitch;
        for (col = 0; col < width; ++col) {
            uint32_t index = 0u, bitpos = source_bit + col, mapped, old;
            const uint8_t *map;
            for (plane = 0; plane < depth; ++plane) {
                uint32_t bit = 0u;
                if (plane_mask & (1u << plane))
                    bit = (upload[(uint64_t)plane * plane_span +
                                  (uint64_t)row * plane_pitch + bitpos / 8u] >>
                           (7u - bitpos % 8u)) & 1u;
                index |= bit << plane;
            }
            map = upload + map_offset + index * 4u;
            mapped = ((uint32_t)map[0] << 24) | ((uint32_t)map[1] << 16) |
                     ((uint32_t)map[2] << 8) | map[3];
            old = load_pixel(out, bpp);
            store_pixel(out, bpp, (old & ~color_mask) | (mapped & color_mask));
            out += bpp;
        }
    }
    return 1;
}

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
    s->force_full = 1u;
    s->sprite_colors[0] = 0u;
    s->sprite_image_dirty = 1u;
    s->sprite_state_dirty = 1u;
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
        case RTG_REG_SPRITE_ENABLE: return s->sprite_visible;
        case RTG_REG_SPRITE_XY:
            return ((uint32_t)(uint16_t)s->sprite_x << 16) |
                   (uint16_t)s->sprite_y;
        case RTG_REG_SPRITE_WH:
            return ((uint32_t)s->sprite_w << 16) | s->sprite_h;
        default:                    return 0u;
    }
}

void bellatrix_rtg_scanout_reg_write(BellatrixRtgScanout *s,
                                     uint32_t reg, uint32_t value)
{
    switch (reg) {
        case RTG_REG_ENABLE:
            if (s->enable != (value & 1u)) s->force_full = 1u;
            s->enable = value & 1u; break;
        case RTG_REG_MODE_W:
            if (s->mode_w != value) s->force_full = 1u;
            s->mode_w = value; break;
        case RTG_REG_MODE_H:
            if (s->mode_h != value) s->force_full = 1u;
            s->mode_h = value; break;
        case RTG_REG_FORMAT:
            if (s->format != value) s->force_full = 1u;
            s->format = value; break;
        case RTG_REG_BYTES_PER_ROW:
            if (s->bytes_per_row != value) s->force_full = 1u;
            s->bytes_per_row = value; break;
        case RTG_REG_PAN:
            if (s->pan != value) s->force_full = 1u;
            s->pan = value; break;
        case RTG_REG_PAL_INDEX:     s->pal_index = value & 0xffu; break;
        case RTG_REG_PAL_DATA:
            s->palette[s->pal_index] = value & 0x00ffffffu;
            s->pal_index = (s->pal_index + 1u) & 0xffu;
            s->force_full = 1u;
            break;
        case RTG_REG_SPRITE_ENABLE:
            if (s->sprite_visible != (value & 1u)) s->sprite_state_dirty = 1u;
            s->sprite_visible = value & 1u;
            break;
        case RTG_REG_SPRITE_XY: {
            int16_t x = (int16_t)(value >> 16), y = (int16_t)value;
            if (s->sprite_x != x || s->sprite_y != y) s->sprite_state_dirty = 1u;
            s->sprite_x = x; s->sprite_y = y;
            break;
        }
        case RTG_REG_SPRITE_WH: {
            uint16_t w = (uint16_t)(value >> 16), h = (uint16_t)value;
            if (w > RTG_SPRITE_MAX_W) w = RTG_SPRITE_MAX_W;
            if (h > RTG_SPRITE_MAX_H) h = RTG_SPRITE_MAX_H;
            if (s->sprite_w != w || s->sprite_h != h) {
                s->sprite_w = w; s->sprite_h = h;
                s->sprite_image_dirty = 1u; s->sprite_state_dirty = 1u;
            }
            break;
        }
        case RTG_REG_SPRITE_COLOR_INDEX:
            s->pal_index = value & 3u;
            break;
        case RTG_REG_SPRITE_COLOR_DATA:
            s->sprite_colors[s->pal_index & 3u] = value & 0x00ffffffu;
            s->sprite_image_dirty = 1u; s->sprite_state_dirty = 1u;
            break;
        case RTG_REG_SPRITE_UPLOAD_RESET:
            s->sprite_upload_pos = 0u;
            memset(s->sprite_indices, 0, sizeof(s->sprite_indices));
            s->sprite_image_dirty = 1u; s->sprite_state_dirty = 1u;
            break;
        case RTG_REG_SPRITE_UPLOAD_DATA: {
            uint32_t i;
            for (i = 0u; i < 16u &&
                 s->sprite_upload_pos < sizeof(s->sprite_indices); ++i) {
                s->sprite_indices[s->sprite_upload_pos++] =
                    (uint8_t)((value >> (30u - i * 2u)) & 3u);
            }
            s->sprite_image_dirty = 1u; s->sprite_state_dirty = 1u;
            break;
        }
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
    uint32_t w, h, bpr, pixel_bytes, columns, rows, tx, ty;
    uint64_t required, dirty_pixels = 0u;
    int full;
    uint8_t sprite_image_changed;

    if (!out || !bellatrix_rtg_scanout_active(s) || s->pan >= s->vram_size)
        return 0;

    memset(out, 0, sizeof(*out));

    w = s->mode_w;
    h = s->mode_h;
    bpr = s->bytes_per_row;
    pixel_bytes = format_bytes(s->format);
    required = (uint64_t)(h - 1u) * bpr + (uint64_t)w * pixel_bytes;
    if (required > (uint64_t)s->vram_size - s->pan ||
        (uint64_t)w * h * 4u > s->frame_capacity)
        return 0;

    columns = (w + RTG_DIRTY_TILE_W - 1u) / RTG_DIRTY_TILE_W;
    rows = (h + RTG_DIRTY_TILE_H - 1u) / RTG_DIRTY_TILE_H;
    if ((uint64_t)columns * rows > RTG_DIRTY_MAX_TILES)
        return 0;
    full = s->force_full || !s->hashes_valid ||
           s->tile_columns != columns || s->tile_rows != rows;

    for (ty = 0u; ty < rows; ++ty) {
        uint32_t run_start = columns;
        for (tx = 0u; tx <= columns; ++tx) {
            int changed = 0;
            if (tx < columns) {
                uint32_t x = tx * RTG_DIRTY_TILE_W;
                uint32_t y = ty * RTG_DIRTY_TILE_H;
                uint32_t tw = w - x < RTG_DIRTY_TILE_W ? w - x : RTG_DIRTY_TILE_W;
                uint32_t th = h - y < RTG_DIRTY_TILE_H ? h - y : RTG_DIRTY_TILE_H;
                uint32_t index = ty * columns + tx;
                uint32_t hash = hash_tile(s->vram + s->pan, bpr, x, y,
                                          tw, th, pixel_bytes);
                changed = full || s->tile_hash[index] != hash;
                s->tile_hash[index] = hash;
            }
            if (changed && run_start == columns) {
                run_start = tx;
            } else if (!changed && run_start != columns) {
                uint32_t x = run_start * RTG_DIRTY_TILE_W;
                uint32_t y = ty * RTG_DIRTY_TILE_H;
                uint32_t rw = tx * RTG_DIRTY_TILE_W - x;
                uint32_t rh = h - y < RTG_DIRTY_TILE_H ? h - y : RTG_DIRTY_TILE_H;
                if (x + rw > w) rw = w - x;
                dirty_pixels += (uint64_t)rw * rh;
                if (!full && out->dirty_count < RTG_DIRTY_MAX_RECTS) {
                    BellatrixRtgRect *r = &out->dirty[out->dirty_count++];
                    r->x = (uint16_t)x; r->y = (uint16_t)y;
                    r->w = (uint16_t)rw; r->h = (uint16_t)rh;
                } else if (!full) {
                    full = 1;
                }
                run_start = columns;
            }
        }
    }

    if (dirty_pixels * 100u >= (uint64_t)w * h * 80u)
        full = 1;
    if (full) {
        convert_rect(s, 0u, 0u, w, h);
        out->dirty_count = 0u;
        out->full_update = 1u;
        out->changed = 1u;
    } else {
        uint32_t i;
        for (i = 0u; i < out->dirty_count; ++i) {
            BellatrixRtgRect *r = &out->dirty[i];
            convert_rect(s, r->x, r->y, r->w, r->h);
        }
        out->changed = out->dirty_count != 0u;
    }

    s->tile_columns = (uint16_t)columns;
    s->tile_rows = (uint16_t)rows;
    s->hashes_valid = 1u;
    s->force_full = 0u;

    sprite_image_changed = s->sprite_image_dirty;
    out->pixels = s->frame;
    out->width = w;
    out->height = h;
    out->pitch = w * 4u;
    update_sprite_rgba(s);
    out->sprite_pixels = s->sprite_rgba;
    out->sprite_x = s->sprite_x;
    out->sprite_y = s->sprite_y;
    out->sprite_w = s->sprite_w;
    out->sprite_h = s->sprite_h;
    out->sprite_visible = s->sprite_visible;
    out->sprite_changed = s->sprite_state_dirty;
    out->sprite_image_changed = sprite_image_changed;
    s->sprite_state_dirty = 0u;
    return 1;
}
