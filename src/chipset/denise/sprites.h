#ifndef BELLATRIX_DENISE_SPRITES_H
#define BELLATRIX_DENISE_SPRITES_H

#include <stdint.h>
#include <stdbool.h>

#define AMIGA_SPRITE_COUNT 8

typedef struct AmigaSprite {
    uint16_t pos;      /* SPRxPOS  - VSTART[7:0], HSTART[8:1] */
    uint16_t ctl;      /* SPRxCTL  - VSTOP[7:0], attach, hstart bit0, vstart/vstop high bits */
    uint16_t data_a;   /* SPRxDATA - bitplane A */
    uint16_t data_b;   /* SPRxDATB - bitplane B */

    uint16_t ptr_hi;
    uint16_t ptr_lo;
    uint32_t ptr;

    bool armed;
    bool dma_active;
    bool line_active;
    uint8_t dma_words_pending;

    uint16_t shift_a;
    uint16_t shift_b;
    int shift_count;
} AmigaSprite;

typedef struct DeniseSprites {
    AmigaSprite spr[AMIGA_SPRITE_COUNT];

    uint16_t collision_mask;
} DeniseSprites;

void denise_sprites_init(DeniseSprites *sprites);
void denise_sprites_reset(DeniseSprites *sprites);

void denise_sprite_write_pos(DeniseSprites *sprites, int index, uint16_t value);
void denise_sprite_write_ctl(DeniseSprites *sprites, int index, uint16_t value);
void denise_sprite_write_data_a(DeniseSprites *sprites, int index, uint16_t value);
void denise_sprite_write_data_b(DeniseSprites *sprites, int index, uint16_t value);

void denise_sprite_write_ptr_hi(DeniseSprites *sprites, int index, uint16_t value);
void denise_sprite_write_ptr_lo(DeniseSprites *sprites, int index, uint16_t value);

uint16_t denise_sprite_read_pos(const DeniseSprites *sprites, int index);
uint16_t denise_sprite_read_ctl(const DeniseSprites *sprites, int index);
uint16_t denise_sprite_read_data_a(const DeniseSprites *sprites, int index);
uint16_t denise_sprite_read_data_b(const DeniseSprites *sprites, int index);

uint32_t denise_sprite_get_ptr(const DeniseSprites *sprites, int index);

bool denise_sprite_vertical_active(const DeniseSprites *sprites, int index, int vpos);
int denise_sprite_hstart(const DeniseSprites *sprites, int index);
int denise_sprite_vstart(const DeniseSprites *sprites, int index);
int denise_sprite_vstop(const DeniseSprites *sprites, int index);
bool denise_sprite_attached(const DeniseSprites *sprites, int index);

void denise_sprite_load_words(DeniseSprites *sprites, int index, uint16_t data_a, uint16_t data_b);
void denise_sprite_begin_line(DeniseSprites *sprites, int vpos);
void denise_sprite_step_pixel(DeniseSprites *sprites);
uint32_t denise_sprites_dma_request_mask(const DeniseSprites *sprites);
void denise_sprite_dma_service(DeniseSprites *sprites, int index, uint16_t data_word);

uint8_t denise_sprites_pixel(const DeniseSprites *sprites, int hpos, uint8_t *sprite_index_out);

uint16_t denise_sprites_get_collision_mask(const DeniseSprites *sprites);
void denise_sprites_clear_collision_mask(DeniseSprites *sprites, uint16_t mask);

#endif
