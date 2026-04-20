#include "chipset/agnus/dma.h"

#include <string.h>

static inline bool dma_has_master(const AgnusDMA *dma)
{
    return (dma->dmacon & AGNUS_DMACON_DMAEN) != 0;
}

static inline bool dma_has_disk(const AgnusDMA *dma)
{
    return (dma->dmacon & AGNUS_DMACON_DSKEN) != 0;
}

static inline bool dma_has_audio(const AgnusDMA *dma, unsigned int channel)
{
    switch (channel)
    {
        case 0: return (dma->dmacon & AGNUS_DMACON_AUD0EN) != 0;
        case 1: return (dma->dmacon & AGNUS_DMACON_AUD1EN) != 0;
        case 2: return (dma->dmacon & AGNUS_DMACON_AUD2EN) != 0;
        case 3: return (dma->dmacon & AGNUS_DMACON_AUD3EN) != 0;
        default: return false;
    }
}

static inline bool dma_has_sprite(const AgnusDMA *dma)
{
    return (dma->dmacon & AGNUS_DMACON_SPREN) != 0;
}

static inline bool dma_has_bitplane(const AgnusDMA *dma)
{
    return (dma->dmacon & AGNUS_DMACON_BPLEN) != 0;
}

static inline bool dma_has_copper(const AgnusDMA *dma)
{
    return (dma->dmacon & AGNUS_DMACON_COPEN) != 0;
}

static inline bool dma_has_blitter(const AgnusDMA *dma)
{
    return (dma->dmacon & AGNUS_DMACON_BLTEN) != 0;
}

static inline bool dma_blitter_busy(const AgnusDMA *dma)
{
    if (dma->ops.blitter_busy)
        return dma->ops.blitter_busy(dma->ctx);

    return dma->blitter_busy_latched;
}

static inline bool dma_blitter_zero(const AgnusDMA *dma)
{
    if (dma->ops.blitter_zero)
        return dma->ops.blitter_zero(dma->ctx);

    return dma->blitter_zero_latched;
}

static inline uint32_t dma_filter_requests(const AgnusDMA *dma, uint32_t req)
{
    uint32_t out = 0;

    if (!dma_has_master(dma))
        return 0;

    if (dma_has_disk(dma) && (req & AGNUS_DMA_REQ_DISK))
        out |= AGNUS_DMA_REQ_DISK;

    if (dma_has_audio(dma, 0) && (req & AGNUS_DMA_REQ_AUDIO0))
        out |= AGNUS_DMA_REQ_AUDIO0;
    if (dma_has_audio(dma, 1) && (req & AGNUS_DMA_REQ_AUDIO1))
        out |= AGNUS_DMA_REQ_AUDIO1;
    if (dma_has_audio(dma, 2) && (req & AGNUS_DMA_REQ_AUDIO2))
        out |= AGNUS_DMA_REQ_AUDIO2;
    if (dma_has_audio(dma, 3) && (req & AGNUS_DMA_REQ_AUDIO3))
        out |= AGNUS_DMA_REQ_AUDIO3;

    if (dma_has_sprite(dma))
    {
        out |= req & (
            AGNUS_DMA_REQ_SPRITE0 |
            AGNUS_DMA_REQ_SPRITE1 |
            AGNUS_DMA_REQ_SPRITE2 |
            AGNUS_DMA_REQ_SPRITE3 |
            AGNUS_DMA_REQ_SPRITE4 |
            AGNUS_DMA_REQ_SPRITE5 |
            AGNUS_DMA_REQ_SPRITE6 |
            AGNUS_DMA_REQ_SPRITE7
        );
    }

    if (dma_has_bitplane(dma))
    {
        out |= req & (
            AGNUS_DMA_REQ_BITPLANE1 |
            AGNUS_DMA_REQ_BITPLANE2 |
            AGNUS_DMA_REQ_BITPLANE3 |
            AGNUS_DMA_REQ_BITPLANE4 |
            AGNUS_DMA_REQ_BITPLANE5 |
            AGNUS_DMA_REQ_BITPLANE6
        );
    }

    if (dma_has_copper(dma) && (req & AGNUS_DMA_REQ_COPPER))
        out |= AGNUS_DMA_REQ_COPPER;

    if (dma_has_blitter(dma) && dma_blitter_busy(dma) && (req & AGNUS_DMA_REQ_BLITTER))
        out |= AGNUS_DMA_REQ_BLITTER;

    return out;
}

/*
 * Política de prioridade explícita e determinística.
 *
 * Não tenta ser uma reprodução completa do microagendamento do hardware;
 * ela organiza o básico do sistema sem espalhar a arbitragem pelo projeto.
 *
 * O ponto importante é: a arbitragem fica aqui, dentro do DMA do Agnus.
 */
static AgnusDMARequest dma_pick_request(const AgnusDMA *dma, uint32_t req)
{
    static const AgnusDMARequest normal_priority[] =
    {
        AGNUS_DMA_REQ_DISK,
        AGNUS_DMA_REQ_AUDIO3,
        AGNUS_DMA_REQ_AUDIO2,
        AGNUS_DMA_REQ_AUDIO1,
        AGNUS_DMA_REQ_AUDIO0,
        AGNUS_DMA_REQ_SPRITE7,
        AGNUS_DMA_REQ_SPRITE6,
        AGNUS_DMA_REQ_SPRITE5,
        AGNUS_DMA_REQ_SPRITE4,
        AGNUS_DMA_REQ_SPRITE3,
        AGNUS_DMA_REQ_SPRITE2,
        AGNUS_DMA_REQ_SPRITE1,
        AGNUS_DMA_REQ_SPRITE0,
        AGNUS_DMA_REQ_BITPLANE6,
        AGNUS_DMA_REQ_BITPLANE5,
        AGNUS_DMA_REQ_BITPLANE4,
        AGNUS_DMA_REQ_BITPLANE3,
        AGNUS_DMA_REQ_BITPLANE2,
        AGNUS_DMA_REQ_BITPLANE1,
        AGNUS_DMA_REQ_COPPER,
        AGNUS_DMA_REQ_BLITTER
    };

    static const AgnusDMARequest blitter_nasty_priority[] =
    {
        AGNUS_DMA_REQ_BLITTER,
        AGNUS_DMA_REQ_DISK,
        AGNUS_DMA_REQ_AUDIO3,
        AGNUS_DMA_REQ_AUDIO2,
        AGNUS_DMA_REQ_AUDIO1,
        AGNUS_DMA_REQ_AUDIO0,
        AGNUS_DMA_REQ_SPRITE7,
        AGNUS_DMA_REQ_SPRITE6,
        AGNUS_DMA_REQ_SPRITE5,
        AGNUS_DMA_REQ_SPRITE4,
        AGNUS_DMA_REQ_SPRITE3,
        AGNUS_DMA_REQ_SPRITE2,
        AGNUS_DMA_REQ_SPRITE1,
        AGNUS_DMA_REQ_SPRITE0,
        AGNUS_DMA_REQ_BITPLANE6,
        AGNUS_DMA_REQ_BITPLANE5,
        AGNUS_DMA_REQ_BITPLANE4,
        AGNUS_DMA_REQ_BITPLANE3,
        AGNUS_DMA_REQ_BITPLANE2,
        AGNUS_DMA_REQ_BITPLANE1,
        AGNUS_DMA_REQ_COPPER
    };

    const AgnusDMARequest *table = normal_priority;
    const unsigned int count_normal = (unsigned int)(sizeof(normal_priority) / sizeof(normal_priority[0]));
    const unsigned int count_nasty = (unsigned int)(sizeof(blitter_nasty_priority) / sizeof(blitter_nasty_priority[0]));
    unsigned int i;
    unsigned int count;

    if ((dma->dmacon & AGNUS_DMACON_BLTPRI) != 0)
    {
        table = blitter_nasty_priority;
        count = count_nasty;
    }
    else
    {
        count = count_normal;
    }

    for (i = 0; i < count; ++i)
    {
        if (req & (uint32_t)table[i])
            return table[i];
    }

    return AGNUS_DMA_REQ_NONE;
}

void agnus_dma_init(AgnusDMA *dma, void *ctx, const AgnusDMAOps *ops)
{
    memset(dma, 0, sizeof(*dma));

    dma->ctx = ctx;

    if (ops)
        dma->ops = *ops;

    dma->dmacon = 0;
    dma->blitter_busy_latched = false;
    dma->blitter_zero_latched = true;
    dma->slot_counter = 0;
    dma->grant_counter = 0;
    dma->last_grant = AGNUS_DMA_REQ_NONE;
}

void agnus_dma_reset(AgnusDMA *dma)
{
    AgnusDMAOps ops = dma->ops;
    void *ctx = dma->ctx;

    memset(dma, 0, sizeof(*dma));

    dma->ctx = ctx;
    dma->ops = ops;

    dma->dmacon = 0;
    dma->blitter_busy_latched = false;
    dma->blitter_zero_latched = true;
    dma->slot_counter = 0;
    dma->grant_counter = 0;
    dma->last_grant = AGNUS_DMA_REQ_NONE;
}

uint16_t agnus_dma_read_dmaconr(const AgnusDMA *dma)
{
    uint16_t value = dma->dmacon & 0x07ff;

    if (dma_blitter_busy(dma))
        value |= AGNUS_DMACON_BBUSY;

    if (dma_blitter_zero(dma))
        value |= AGNUS_DMACON_BZERO;

    return value;
}

void agnus_dma_write_dmacon(AgnusDMA *dma, uint16_t value)
{
    uint16_t mask = (uint16_t)(value & 0x07ff);

    if (value & AGNUS_DMACON_SETCLR)
        dma->dmacon |= mask;
    else
        dma->dmacon &= (uint16_t)~mask;
}

bool agnus_dma_master_enabled(const AgnusDMA *dma)
{
    return dma_has_master(dma);
}

bool agnus_dma_disk_enabled(const AgnusDMA *dma)
{
    return dma_has_disk(dma);
}

bool agnus_dma_audio_enabled(const AgnusDMA *dma, unsigned int channel)
{
    return dma_has_audio(dma, channel);
}

bool agnus_dma_sprite_enabled(const AgnusDMA *dma)
{
    return dma_has_sprite(dma);
}

bool agnus_dma_bitplane_enabled(const AgnusDMA *dma)
{
    return dma_has_bitplane(dma);
}

bool agnus_dma_copper_enabled(const AgnusDMA *dma)
{
    return dma_has_copper(dma);
}

bool agnus_dma_blitter_enabled(const AgnusDMA *dma)
{
    return dma_has_blitter(dma);
}

bool agnus_dma_blitter_nasty(const AgnusDMA *dma)
{
    return (dma->dmacon & AGNUS_DMACON_BLTPRI) != 0;
}

void agnus_dma_set_blitter_busy(AgnusDMA *dma, bool busy)
{
    dma->blitter_busy_latched = busy;
}

void agnus_dma_set_blitter_zero(AgnusDMA *dma, bool zero)
{
    dma->blitter_zero_latched = zero;
}

void agnus_dma_step(AgnusDMA *dma, uint32_t slots)
{
    uint32_t i;

    for (i = 0; i < slots; ++i)
    {
        uint32_t req = 0;
        uint32_t filtered = 0;
        AgnusDMARequest grant = AGNUS_DMA_REQ_NONE;

        if (dma->ops.advance_slot)
            dma->ops.advance_slot(dma->ctx);

        dma->slot_counter++;

        if (!dma->ops.query_requests)
        {
            dma->last_grant = AGNUS_DMA_REQ_NONE;
            continue;
        }

        req = dma->ops.query_requests(dma->ctx);
        filtered = dma_filter_requests(dma, req);
        grant = dma_pick_request(dma, filtered);

        dma->last_grant = grant;

        if (grant == AGNUS_DMA_REQ_NONE)
            continue;

        dma->grant_counter++;

        if (dma->ops.service_request)
            dma->ops.service_request(dma->ctx, grant);
    }
}