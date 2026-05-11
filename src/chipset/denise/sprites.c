#include "sprites.h"

#include <string.h>

static bool valid_sprite_index(int index)
{
    return index >= 0 && index < AMIGA_SPRITE_COUNT;
}

static uint32_t sprite_request_bit(int index)
{
    return (index >= 0 && index < AMIGA_SPRITE_COUNT) ? (1u << (index + 5)) : 0u;
}

void denise_sprites_init(DeniseSprites *sprites)
{
    if (!sprites) {
        return;
    }

    memset(sprites, 0, sizeof(*sprites));
}

void denise_sprites_reset(DeniseSprites *sprites)
{
    denise_sprites_init(sprites);
}

static void update_sprite_ptr(AmigaSprite *spr)
{
    spr->ptr = (((uint32_t)spr->ptr_hi & 0xffffu) << 16) |
               ((uint32_t)spr->ptr_lo & 0xfffeu);
}

void denise_sprite_write_pos(DeniseSprites *sprites, int index, uint16_t value)
{
    if (!sprites || !valid_sprite_index(index)) {
        return;
    }

    sprites->spr[index].pos = value;
}

void denise_sprite_write_ctl(DeniseSprites *sprites, int index, uint16_t value)
{
    if (!sprites || !valid_sprite_index(index)) {
        return;
    }

    sprites->spr[index].ctl = value;
}

void denise_sprite_write_data_a(DeniseSprites *sprites, int index, uint16_t value)
{
    if (!sprites || !valid_sprite_index(index)) {
        return;
    }

    AmigaSprite *spr = &sprites->spr[index];
    spr->data_a = value;
    spr->shift_a = value;
    spr->shift_count = 16;
    spr->armed = true;
}

void denise_sprite_write_data_b(DeniseSprites *sprites, int index, uint16_t value)
{
    if (!sprites || !valid_sprite_index(index)) {
        return;
    }

    AmigaSprite *spr = &sprites->spr[index];
    spr->data_b = value;
    spr->shift_b = value;
    spr->shift_count = 16;
    spr->armed = true;
}

void denise_sprite_write_ptr_hi(DeniseSprites *sprites, int index, uint16_t value)
{
    if (!sprites || !valid_sprite_index(index)) {
        return;
    }

    AmigaSprite *spr = &sprites->spr[index];
    spr->ptr_hi = value;
    update_sprite_ptr(spr);
}

void denise_sprite_write_ptr_lo(DeniseSprites *sprites, int index, uint16_t value)
{
    if (!sprites || !valid_sprite_index(index)) {
        return;
    }

    AmigaSprite *spr = &sprites->spr[index];
    spr->ptr_lo = value & 0xfffeu;
    update_sprite_ptr(spr);
}

uint16_t denise_sprite_read_pos(const DeniseSprites *sprites, int index)
{
    if (!sprites || !valid_sprite_index(index)) {
        return 0xffff;
    }

    return sprites->spr[index].pos;
}

uint16_t denise_sprite_read_ctl(const DeniseSprites *sprites, int index)
{
    if (!sprites || !valid_sprite_index(index)) {
        return 0xffff;
    }

    return sprites->spr[index].ctl;
}

uint16_t denise_sprite_read_data_a(const DeniseSprites *sprites, int index)
{
    if (!sprites || !valid_sprite_index(index)) {
        return 0xffff;
    }

    return sprites->spr[index].data_a;
}

uint16_t denise_sprite_read_data_b(const DeniseSprites *sprites, int index)
{
    if (!sprites || !valid_sprite_index(index)) {
        return 0xffff;
    }

    return sprites->spr[index].data_b;
}

uint32_t denise_sprite_get_ptr(const DeniseSprites *sprites, int index)
{
    if (!sprites || !valid_sprite_index(index)) {
        return 0;
    }

    return sprites->spr[index].ptr;
}

int denise_sprite_hstart(const DeniseSprites *sprites, int index)
{
    if (!sprites || !valid_sprite_index(index)) {
        return 0;
    }

    const AmigaSprite *spr = &sprites->spr[index];

    return ((spr->pos & 0x00ffu) << 1) | (spr->ctl & 0x0001u);
}

int denise_sprite_vstart(const DeniseSprites *sprites, int index)
{
    if (!sprites || !valid_sprite_index(index)) {
        return 0;
    }

    const AmigaSprite *spr = &sprites->spr[index];

    return ((spr->pos >> 8) & 0x00ffu) | ((spr->ctl & 0x0004u) << 6);
}

int denise_sprite_vstop(const DeniseSprites *sprites, int index)
{
    if (!sprites || !valid_sprite_index(index)) {
        return 0;
    }

    const AmigaSprite *spr = &sprites->spr[index];

    return ((spr->ctl >> 8) & 0x00ffu) | ((spr->ctl & 0x0002u) << 7);
}

bool denise_sprite_attached(const DeniseSprites *sprites, int index)
{
    if (!sprites || !valid_sprite_index(index)) {
        return false;
    }

    return (sprites->spr[index].ctl & 0x0080u) != 0;
}

bool denise_sprite_vertical_active(const DeniseSprites *sprites, int index, int vpos)
{
    if (!sprites || !valid_sprite_index(index)) {
        return false;
    }

    int vstart = denise_sprite_vstart(sprites, index);
    int vstop = denise_sprite_vstop(sprites, index);

    return vpos >= vstart && vpos < vstop;
}

void denise_sprite_load_words(DeniseSprites *sprites, int index, uint16_t data_a, uint16_t data_b)
{
    if (!sprites || !valid_sprite_index(index)) {
        return;
    }

    AmigaSprite *spr = &sprites->spr[index];

    spr->data_a = data_a;
    spr->data_b = data_b;

    spr->shift_a = data_a;
    spr->shift_b = data_b;
    spr->shift_count = 16;
    spr->armed = true;
    spr->dma_active = true;
    spr->dma_words_pending = 0;
}

void denise_sprite_begin_line(DeniseSprites *sprites, int vpos)
{
    if (!sprites) {
        return;
    }

    for (int i = 0; i < AMIGA_SPRITE_COUNT; i++) {
        AmigaSprite *spr = &sprites->spr[i];
        bool vertical_active = denise_sprite_vertical_active(sprites, i, vpos);

        spr->line_active = vertical_active;
        spr->dma_active = false;
        spr->dma_words_pending = 0;

        if (!vertical_active) {
            spr->shift_count = 0;
            spr->armed = false;
            continue;
        }

        if (spr->ptr != 0u) {
            spr->shift_count = 0;
            spr->armed = false;
            spr->dma_words_pending = 2;
            continue;
        }

        if (spr->data_a != 0u || spr->data_b != 0u) {
            spr->shift_a = spr->data_a;
            spr->shift_b = spr->data_b;
            spr->shift_count = 16;
            spr->armed = true;
            spr->dma_active = true;
        } else {
            spr->shift_count = 0;
            spr->armed = false;
        }
    }
}

void denise_sprite_step_pixel(DeniseSprites *sprites)
{
    if (!sprites) {
        return;
    }

    for (int i = 0; i < AMIGA_SPRITE_COUNT; i++) {
        AmigaSprite *spr = &sprites->spr[i];

        if (!spr->armed || spr->shift_count <= 0) {
            continue;
        }

        spr->shift_a <<= 1;
        spr->shift_b <<= 1;
        spr->shift_count--;

        if (spr->shift_count == 0) {
            spr->armed = false;
        }
    }
}

uint32_t denise_sprites_dma_request_mask(const DeniseSprites *sprites)
{
    uint32_t mask = 0;

    if (!sprites) {
        return 0;
    }

    for (int i = 0; i < AMIGA_SPRITE_COUNT; ++i) {
        const AmigaSprite *spr = &sprites->spr[i];

        if (spr->line_active && spr->dma_words_pending > 0) {
            mask |= sprite_request_bit(i);
        }
    }

    return mask;
}

void denise_sprite_dma_service(DeniseSprites *sprites, int index, uint16_t data_word)
{
    AmigaSprite *spr;

    if (!sprites || !valid_sprite_index(index)) {
        return;
    }

    spr = &sprites->spr[index];
    if (!spr->line_active || spr->dma_words_pending == 0) {
        return;
    }

    if (spr->dma_words_pending == 2) {
        spr->data_a = data_word;
        spr->ptr = (spr->ptr + 2u) & 0x00fffffeu;
        spr->dma_words_pending = 1;
        return;
    }

    spr->data_b = data_word;
    spr->ptr = (spr->ptr + 2u) & 0x00fffffeu;
    denise_sprite_load_words(sprites, index, spr->data_a, spr->data_b);
}

static uint8_t sprite_raw_pixel(const AmigaSprite *spr)
{
    if (!spr || !spr->armed || spr->shift_count <= 0) {
        return 0;
    }

    uint8_t bit_a = (spr->shift_a & 0x8000u) ? 1 : 0;
    uint8_t bit_b = (spr->shift_b & 0x8000u) ? 1 : 0;

    return (uint8_t)((bit_b << 1) | bit_a);
}

uint8_t denise_sprites_pixel(const DeniseSprites *sprites, int hpos, uint8_t *sprite_index_out)
{
    if (sprite_index_out) {
        *sprite_index_out = 0xff;
    }

    if (!sprites) {
        return 0;
    }

    for (int i = 0; i < AMIGA_SPRITE_COUNT; i++) {
        const AmigaSprite *spr = &sprites->spr[i];

        if (!spr->dma_active || !spr->armed) {
            continue;
        }

        int hstart = denise_sprite_hstart(sprites, i);
        int rel = hpos - hstart;

        if (rel < 0 || rel >= 16) {
            continue;
        }

        uint8_t pix = sprite_raw_pixel(spr);

        if (pix != 0) {
            if (sprite_index_out) {
                *sprite_index_out = (uint8_t)i;
            }

            return pix;
        }
    }

    return 0;
}

uint16_t denise_sprites_get_collision_mask(const DeniseSprites *sprites)
{
    if (!sprites) {
        return 0;
    }

    return sprites->collision_mask;
}

void denise_sprites_clear_collision_mask(DeniseSprites *sprites, uint16_t mask)
{
    if (!sprites) {
        return;
    }

    sprites->collision_mask &= (uint16_t)~mask;
}
