#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMACON bits
 *
 * bit 15 = SET/CLR na escrita
 * bit 14 = BBUSY   (somente leitura)
 * bit 13 = BZERO   (somente leitura)
 * bit 10 = BLTPRI
 * bit  9 = DMAEN
 * bit  8 = BPLEN
 * bit  7 = COPEN
 * bit  6 = BLTEN
 * bit  5 = SPREN
 * bit  4 = DSKEN
 * bit  3 = AUD3EN
 * bit  2 = AUD2EN
 * bit  1 = AUD1EN
 * bit  0 = AUD0EN
 */

enum
{
    AGNUS_DMACON_SETCLR = 0x8000,
    AGNUS_DMACON_BBUSY  = 0x4000,
    AGNUS_DMACON_BZERO  = 0x2000,
    AGNUS_DMACON_BLTPRI = 0x0400,
    AGNUS_DMACON_DMAEN  = 0x0200,
    AGNUS_DMACON_BPLEN  = 0x0100,
    AGNUS_DMACON_COPEN  = 0x0080,
    AGNUS_DMACON_BLTEN  = 0x0040,
    AGNUS_DMACON_SPREN  = 0x0020,
    AGNUS_DMACON_DSKEN  = 0x0010,
    AGNUS_DMACON_AUD3EN = 0x0008,
    AGNUS_DMACON_AUD2EN = 0x0004,
    AGNUS_DMACON_AUD1EN = 0x0002,
    AGNUS_DMACON_AUD0EN = 0x0001
};

/*
 * Pedidos de DMA para o slot corrente.
 *
 * O host (Agnus/beam/raster) informa quais consumidores
 * querem usar o slot corrente. O árbitro resolve quem ganha.
 */
typedef enum AgnusDMARequest
{
    AGNUS_DMA_REQ_NONE      = 0,
    AGNUS_DMA_REQ_DISK      = 1u << 0,
    AGNUS_DMA_REQ_AUDIO0    = 1u << 1,
    AGNUS_DMA_REQ_AUDIO1    = 1u << 2,
    AGNUS_DMA_REQ_AUDIO2    = 1u << 3,
    AGNUS_DMA_REQ_AUDIO3    = 1u << 4,
    AGNUS_DMA_REQ_SPRITE0   = 1u << 5,
    AGNUS_DMA_REQ_SPRITE1   = 1u << 6,
    AGNUS_DMA_REQ_SPRITE2   = 1u << 7,
    AGNUS_DMA_REQ_SPRITE3   = 1u << 8,
    AGNUS_DMA_REQ_SPRITE4   = 1u << 9,
    AGNUS_DMA_REQ_SPRITE5   = 1u << 10,
    AGNUS_DMA_REQ_SPRITE6   = 1u << 11,
    AGNUS_DMA_REQ_SPRITE7   = 1u << 12,
    AGNUS_DMA_REQ_BITPLANE1 = 1u << 13,
    AGNUS_DMA_REQ_BITPLANE2 = 1u << 14,
    AGNUS_DMA_REQ_BITPLANE3 = 1u << 15,
    AGNUS_DMA_REQ_BITPLANE4 = 1u << 16,
    AGNUS_DMA_REQ_BITPLANE5 = 1u << 17,
    AGNUS_DMA_REQ_BITPLANE6 = 1u << 18,
    AGNUS_DMA_REQ_COPPER    = 1u << 19,
    AGNUS_DMA_REQ_BLITTER   = 1u << 20
} AgnusDMARequest;

typedef struct AgnusDMAOps
{
    /*
     * Avança o tempo/raster/slot do host em 1 unidade de DMA.
     * Esse é o ponto em que o Agnus real faz o mundo andar.
     */
    void (*advance_slot)(void *ctx);

    /*
     * Informa quem deseja usar o slot corrente.
     */
    uint32_t (*query_requests)(void *ctx);

    /*
     * Executa o consumidor que ganhou o slot.
     */
    void (*service_request)(void *ctx, AgnusDMARequest request);

    /*
     * Permite ao host refletir se o blitter está busy/zero.
     * Se não existir, o estado interno do DMA é usado.
     */
    bool (*blitter_busy)(void *ctx);
    bool (*blitter_zero)(void *ctx);
} AgnusDMAOps;

typedef struct AgnusDMA
{
    void *ctx;
    AgnusDMAOps ops;

    uint16_t dmacon;

    bool blitter_busy_latched;
    bool blitter_zero_latched;

    uint64_t slot_counter;
    uint64_t grant_counter;

    AgnusDMARequest last_grant;
} AgnusDMA;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void agnus_dma_init(AgnusDMA *dma, void *ctx, const AgnusDMAOps *ops);
void agnus_dma_reset(AgnusDMA *dma);

/* ------------------------------------------------------------------------- */
/* DMACON                                                                    */
/* ------------------------------------------------------------------------- */

uint16_t agnus_dma_read_dmaconr(const AgnusDMA *dma);
void     agnus_dma_write_dmacon(AgnusDMA *dma, uint16_t value);

/* ------------------------------------------------------------------------- */
/* status helpers                                                            */
/* ------------------------------------------------------------------------- */

bool agnus_dma_master_enabled(const AgnusDMA *dma);
bool agnus_dma_disk_enabled(const AgnusDMA *dma);
bool agnus_dma_audio_enabled(const AgnusDMA *dma, unsigned int channel);
bool agnus_dma_sprite_enabled(const AgnusDMA *dma);
bool agnus_dma_bitplane_enabled(const AgnusDMA *dma);
bool agnus_dma_copper_enabled(const AgnusDMA *dma);
bool agnus_dma_blitter_enabled(const AgnusDMA *dma);
bool agnus_dma_blitter_nasty(const AgnusDMA *dma);

/* ------------------------------------------------------------------------- */
/* blitter status                                                            */
/* ------------------------------------------------------------------------- */

void agnus_dma_set_blitter_busy(AgnusDMA *dma, bool busy);
void agnus_dma_set_blitter_zero(AgnusDMA *dma, bool zero);

/* ------------------------------------------------------------------------- */
/* execution                                                                 */
/* ------------------------------------------------------------------------- */

void agnus_dma_step(AgnusDMA *dma, uint32_t slots);

#ifdef __cplusplus
}
#endif