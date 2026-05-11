// src/chipset/agnus/agnus.c

#include "agnus.h"

#include "copper/copper.h"
#include "copper/copper_regs.h"
#include "copper/copper_service.h"

#include "chipset/denise/denise.h"
#include "chipset/paula/paula.h"
#include "debug/cpu_pc.h"
#include "host/pal.h"
#include "support.h"

extern uint16_t *framebuffer;
extern uint32_t pitch;
extern uint32_t fb_width;
extern uint32_t fb_height;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

#define CHIP_RAM_ADDR_MASK BELLATRIX_CHIP_RAM_MASK
#define CHIP_RAM_WORD_MASK (BELLATRIX_CHIP_RAM_MASK & ~1u)

static bool agnus_dma_blitter_busy_cb(void *ctx)
{
    AgnusState *s = (AgnusState *)ctx;
    return s ? blitter_is_busy(&s->blitter) : false;
}

static bool agnus_dma_blitter_zero_cb(void *ctx)
{
    AgnusState *s = (AgnusState *)ctx;
    return s ? s->blitter.zero : true;
}

static bool agnus_dma_bitplane_allowed_cb(void *ctx)
{
    AgnusState *s = (AgnusState *)ctx;
    return s ? bitplanes_dma_allowed(s) : false;
}

static void agnus_begin_denise_line(AgnusState *s, uint32_t prev_vpos)
{
    if (!s || !s->denise)
        return;

    if (s->beam.vpos == prev_vpos)
        return;

    denise_sprite_begin_line(&s->denise->sprites, (int)s->beam.vpos);
}

static uint32_t agnus_dma_query_requests_cb(void *ctx)
{
    AgnusState *s = (AgnusState *)ctx;
    uint32_t req = 0;

    if (!s)
        return 0;

    if (agnus_dma_copper_enabled(&s->dma) &&
        s->copper_service.enabled &&
        s->copper.state != COPPER_STATE_HALTED &&
        s->copper.state != COPPER_STATE_WAITING_RASTER &&
        s->copper.state != COPPER_STATE_WAITING_BLITTER)
    {
        req |= AGNUS_DMA_REQ_COPPER;
    }

    if (bitplanes_dma_allowed(s))
        req |= bitplanes_dma_request_mask(&s->bitplanes, s);

    if (agnus_dma_blitter_enabled(&s->dma))
        req |= blitter_dma_request_mask(&s->blitter);

    if (s->denise && agnus_dma_sprite_enabled(&s->dma))
        req |= denise_sprites_dma_request_mask(&s->denise->sprites);

    if (s->paula &&
        agnus_dma_disk_enabled(&s->dma) &&
        paula_disk_dma_wants_service(&s->paula->disk))
    {
        req |= AGNUS_DMA_REQ_DISK;
    }

    return req;
}

static void agnus_dma_service_request_cb(void *ctx, AgnusDMARequest request)
{
    AgnusState *s = (AgnusState *)ctx;

    if (!s)
        return;

    switch (request)
    {
    case AGNUS_DMA_REQ_BITPLANE1:
    case AGNUS_DMA_REQ_BITPLANE2:
    case AGNUS_DMA_REQ_BITPLANE3:
    case AGNUS_DMA_REQ_BITPLANE4:
    case AGNUS_DMA_REQ_BITPLANE5:
    case AGNUS_DMA_REQ_BITPLANE6:
        bitplanes_dma_service_next(&s->bitplanes, s);
        break;

    case AGNUS_DMA_REQ_COPPER:
        copper_service_step(&s->copper_service, &s->copper, s, 1);
        break;

    case AGNUS_DMA_REQ_BLITTER:
        blitter_dma_service_grant(&s->blitter, s);
        break;

    case AGNUS_DMA_REQ_DISK:
        if (s->paula)
            paula_disk_dma_service_grant(&s->paula->disk);
        break;

    case AGNUS_DMA_REQ_SPRITE0:
    case AGNUS_DMA_REQ_SPRITE1:
    case AGNUS_DMA_REQ_SPRITE2:
    case AGNUS_DMA_REQ_SPRITE3:
    case AGNUS_DMA_REQ_SPRITE4:
    case AGNUS_DMA_REQ_SPRITE5:
    case AGNUS_DMA_REQ_SPRITE6:
    case AGNUS_DMA_REQ_SPRITE7:
        if (s->denise && s->memory)
        {
            int sprite_index = -1;
            uint32_t ptr;

            switch (request)
            {
            case AGNUS_DMA_REQ_SPRITE0: sprite_index = 0; break;
            case AGNUS_DMA_REQ_SPRITE1: sprite_index = 1; break;
            case AGNUS_DMA_REQ_SPRITE2: sprite_index = 2; break;
            case AGNUS_DMA_REQ_SPRITE3: sprite_index = 3; break;
            case AGNUS_DMA_REQ_SPRITE4: sprite_index = 4; break;
            case AGNUS_DMA_REQ_SPRITE5: sprite_index = 5; break;
            case AGNUS_DMA_REQ_SPRITE6: sprite_index = 6; break;
            case AGNUS_DMA_REQ_SPRITE7: sprite_index = 7; break;
            default: break;
            }

            if (sprite_index >= 0)
            {
                ptr = denise_sprite_get_ptr(&s->denise->sprites, sprite_index);
                denise_sprite_dma_service(&s->denise->sprites,
                                          sprite_index,
                                          bellatrix_chip_read16(s->memory, ptr));
            }
        }
        break;

    default:
        break;
    }
}

static const AgnusDMAOps s_agnus_dma_ops =
{
    .advance_slot = NULL,
    .query_requests = agnus_dma_query_requests_cb,
    .service_request = agnus_dma_service_request_cb,
    .blitter_busy = agnus_dma_blitter_busy_cb,
    .blitter_zero = agnus_dma_blitter_zero_cb,
    .bitplane_allowed = agnus_dma_bitplane_allowed_cb,
};

/* ECS Fat Agnus 8372A PAL — bits [14:8] of VPOSR */
#define AGNUS_CHIP_ID 0x20u

static inline void agnus_get_beam(const AgnusState *s,
                                  uint16_t *vposr_out,
                                  uint16_t *vhposr_out)
{
    uint16_t lof = s->beam.lof ? 0x8000u : 0u;
    uint16_t vpos8 = (uint16_t)((s->beam.vpos >> 8) & 0x01u);

    *vposr_out = (uint16_t)(lof | (AGNUS_CHIP_ID << 8) | vpos8);
    *vhposr_out = (uint16_t)(((s->beam.vpos & 0xFFu) << 8) |
                             (s->beam.hpos & 0xFFu));
}

static inline int agnus_copper_enabled(const AgnusState *s)
{
    uint16_t dmacon = agnus_dmacon_current(s);
    return (dmacon & DMAF_DMAEN) && (dmacon & DMAF_COPEN);
}

static inline uint32_t agnus_bpl_ptr(const AgnusState *s, unsigned plane)
{
    return ((((uint32_t)(s->bplpth[plane] & 0x001Fu)) << 16) |
            ((uint32_t)s->bplptl[plane] & 0xFFFEu));
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void agnus_init(AgnusState *s)
{
    s->dmacon = 0;
    agnus_dma_init(&s->dma, s, &s_agnus_dma_ops);

    beam_init(&s->beam);
    bitplanes_init(&s->bitplanes);

    s->memory = NULL;

    s->diwstrt = 0x2C81u;
    s->diwstop = 0xF4C1u;
    s->ddfstrt = 0x0038u;
    s->ddfstop = 0x00D0u;

    for (int i = 0; i < 6; i++)
    {
        s->bplpth[i] = 0;
        s->bplptl[i] = 0;
    }

    s->bpl1mod = 0;
    s->bpl2mod = 0;
    s->bplcon0 = 0;

    blitter_init(&s->blitter);

    copper_init(&s->copper);
    copper_service_init(&s->copper_service);

    s->denise = NULL;
    s->paula = NULL;
}

void agnus_reset(AgnusState *s)
{
    struct Denise *saved_denise = s->denise;
    struct Paula *saved_paula = s->paula;
    BellatrixMemory *saved_mem = s->memory;

    agnus_init(s);

    s->denise = saved_denise;
    s->paula = saved_paula;
    s->memory = saved_mem;
}

/* ---------------------------------------------------------------------------
 * Wiring
 * ------------------------------------------------------------------------- */

void agnus_attach_denise(AgnusState *s, struct Denise *d)
{
    s->denise = d;
}

void agnus_attach_paula(AgnusState *s, struct Paula *p)
{
    s->paula = p;
}

void agnus_attach_memory(AgnusState *s, BellatrixMemory *m)
{
    s->memory = m;
}

/* ---------------------------------------------------------------------------
 * IRQ forwarding — Paula owns INTREQ/INTENA
 * ------------------------------------------------------------------------- */

void agnus_intreq_set(AgnusState *s, uint16_t bits)
{
    if (s->paula)
        paula_irq_raise(s->paula, bits);
}

void agnus_intreq_clear(AgnusState *s, uint16_t bits)
{
    if (s->paula)
        paula_irq_clear(s->paula, bits);
}

/* ---------------------------------------------------------------------------
 * Busy state
 * ------------------------------------------------------------------------- */

int agnus_blitter_busy(const AgnusState *s)
{
    uint16_t dmacon = agnus_dmacon_current(s);

    if (!blitter_is_busy(&s->blitter))
        return 0;

    if (!(dmacon & DMAF_DMAEN))
        return 0;

    if (!(dmacon & DMAF_BLTEN))
        return 0;

    return 1;
}

/* ---------------------------------------------------------------------------
 * Debug helpers
 * ------------------------------------------------------------------------- */

static void agnus_dump_copper_list(AgnusState *s)
{
    if (!s->memory)
        return;

    uint32_t lc = s->copper.pc & CHIP_RAM_WORD_MASK;
    uint32_t tail = (lc + 0xA0u) & CHIP_RAM_WORD_MASK;

    kprintf("[COPPER-DUMP] lc=%05x : "
            "%04x %04x %04x %04x %04x %04x %04x %04x "
            "%04x %04x %04x %04x\n",
            (unsigned)lc,
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x00u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x02u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x04u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x06u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x08u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x0au) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x0cu) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x0eu) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x10u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x12u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x14u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (lc + 0x16u) & CHIP_RAM_WORD_MASK));

    kprintf("[COPPER-DUMP-TAIL] lc=%05x : "
            "%04x %04x %04x %04x %04x %04x %04x %04x "
            "%04x %04x %04x %04x\n",
            (unsigned)tail,
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x00u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x02u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x04u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x06u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x08u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x0au) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x0cu) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x0eu) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x10u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x12u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x14u) & CHIP_RAM_WORD_MASK),
            (unsigned)bellatrix_chip_read16(s->memory, (tail + 0x16u) & CHIP_RAM_WORD_MASK));
}

static void agnus_log_vbl_enter(AgnusState *s)
{
    uint16_t intena = s->paula ? s->paula->irq.intena : 0u;
    uint16_t intreq = s->paula ? s->paula->irq.intreq : 0u;
    uint16_t pending = (uint16_t)(intena & intreq & 0x3FFFu);
    uint32_t stale_pc = bellatrix_debug_cpu_pc();
    extern volatile uint32_t g_bellatrix_exec_pc;
    extern volatile uint32_t g_bellatrix_fault_pc;

    kprintf("[VBL-ENTER] frame=%u hpos=%u vpos=%u dmacon=0x%04x "
            "intena=0x%04x intreq=0x%04x pending=0x%04x "
            "exec_pc=%08x fault_pc=%08x stale_pc=%08x\n",
            (unsigned)s->beam.frame,
            (unsigned)s->beam.hpos,
            (unsigned)s->beam.vpos,
            (unsigned)agnus_dmacon_current(s),
            (unsigned)intena,
            (unsigned)intreq,
            (unsigned)pending,
            (unsigned)g_bellatrix_exec_pc,
            (unsigned)g_bellatrix_fault_pc,
            (unsigned)stale_pc);

    if (s->memory)
    {
        kprintf("[VBL-VECS] chip[60]=%08x [64]=%08x [68]=%08x "
                "[6c]=%08x [70]=%08x [78]=%08x\n",
                (unsigned)bellatrix_chip_read32(s->memory, 0x60u),
                (unsigned)bellatrix_chip_read32(s->memory, 0x64u),
                (unsigned)bellatrix_chip_read32(s->memory, 0x68u),
                (unsigned)bellatrix_chip_read32(s->memory, 0x6cu),
                (unsigned)bellatrix_chip_read32(s->memory, 0x70u),
                (unsigned)bellatrix_chip_read32(s->memory, 0x78u));
    }
}

/* ---------------------------------------------------------------------------
 * Step
 * ------------------------------------------------------------------------- */

void agnus_step(AgnusState *s, uint64_t ticks)
{
    if (!ticks)
        return;

    while (ticks-- > 0)
    {
        int vbl_before = beam_is_in_vblank(&s->beam);
        uint32_t prev_vpos = s->beam.vpos;

        /*
         * Beam advances first. The new beam position is then used by Copper WAIT
         * polling before bitplanes snapshot line state.
         */
        beam_step(&s->beam, 1);

        if (s->beam.vpos != prev_vpos)
            s->hsync_pulses++;

        agnus_begin_denise_line(s, prev_vpos);

        /*
         * Critical Copper ordering:
         *
         *   beam -> copper poll/wake -> copper execution -> bitplanes
         *
         * This prevents bitplanes from snapshotting BPLCON0/BPLxPT before the
         * MOVE immediately following a WAIT has executed.
         */
        if (agnus_copper_enabled(s))
            copper_service_poll(&s->copper_service, &s->copper, s);

        agnus_dma_step(&s->dma, 1);

        bitplanes_step(&s->bitplanes, s);

        if (bitplanes_line_ready(&s->bitplanes))
        {
            static uint32_t dbg_denise_call = 0;
            if ((dbg_denise_call++ & 63u) == 0)
            {
                kprintf("[AGNUS->DENISE] frame=%u v=%u h=%u bp_v=%d ready=%d nplanes=%d ddf=%d diw=%04x-%04x\n",
                        (unsigned)s->beam.frame,
                        (unsigned)s->beam.vpos,
                        (unsigned)s->beam.hpos,
                        s->bitplanes.line_vpos,
                        bitplanes_line_ready(&s->bitplanes),
                        s->bitplanes.nplanes,
                        s->bitplanes.ddf_words,
                        (unsigned)s->diwstrt,
                        (unsigned)s->diwstop);
            }

            if (s->denise)
                denise_render_line(s->denise, s, &s->bitplanes);

            bitplanes_clear_line_ready(&s->bitplanes);
        }
        else
        {
            static uint32_t dbg_denise_miss = 0;
            if ((dbg_denise_miss++ & 4095u) == 0)
            {
                kprintf("[AGNUS-NO-DENISE] frame=%u v=%u h=%u ready=%d nplanes=%d ddf=%d bp_v=%d diw=%04x-%04x dmacon=%04x\n",
                        (unsigned)s->beam.frame,
                        (unsigned)s->beam.vpos,
                        (unsigned)s->beam.hpos,
                        bitplanes_line_ready(&s->bitplanes),
                        s->bitplanes.nplanes,
                        s->bitplanes.ddf_words,
                        s->bitplanes.line_vpos,
                        (unsigned)s->diwstrt,
                        (unsigned)s->diwstop,
                        (unsigned)agnus_dmacon_current(s));
            }
        }

        if (!vbl_before && beam_is_in_vblank(&s->beam))
        {
            agnus_log_vbl_enter(s);

            agnus_intreq_set(s, PAULA_INT_VERTB);

            kprintf("[FRAME] pre-reload  bplcon0=%04x bpl1=%04x/%04x dmacon=%04x\n",
                    s->denise ? s->denise->bplcon0 : 0,
                    s->bplpth[0],
                    s->bplptl[0],
                    (unsigned)agnus_dmacon_current(s));

            if (agnus_copper_enabled(s))
            {
                copper_service_vbl_reload(&s->copper_service, &s->copper);
                agnus_dump_copper_list(s);
            }
            else
            {
                kprintf("[COPPER] vbl_reload skipped - COPEN off (dmacon=%04x)\n",
                        (unsigned)agnus_dmacon_current(s));
            }

            kprintf("[DENISE] frame=%u bpt0=0x%05x dmacon=0x%04x\n",
                    (unsigned)s->beam.frame,
                    (unsigned)((((uint32_t)(s->bplpth[0] & 0x1Fu)) << 16) |
                               ((uint32_t)(s->bplptl[0] & 0xFFFEu))),
                    (unsigned)agnus_dmacon_current(s));

            if (framebuffer && pitch)
            {
                uint16_t first = framebuffer[0];
                uint16_t mid = framebuffer[(fb_height / 2u) * (pitch / 2u) + (fb_width / 2u)];
                kprintf("[FB-PRE-FLIP] frame=%u first=%04x mid=%04x size=%ux%u pitch=%u\n",
                        (unsigned)s->beam.frame,
                        (unsigned)first,
                        (unsigned)mid,
                        (unsigned)fb_width,
                        (unsigned)fb_height,
                        (unsigned)pitch);
            }

            PAL_Video_Flip();
        }
    }
}

/* ---------------------------------------------------------------------------
 * Bus protocol
 * ------------------------------------------------------------------------- */

int agnus_handles_read(const AgnusState *s, uint32_t addr)
{
    (void)s;

    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;

    uint16_t reg = (uint16_t)(addr & 0x1FEu);

    if (reg == 0x001Cu || reg == 0x001Eu)
        return 0;

    if (reg >= 0x0100u &&
        reg != AGNUS_BPLCON0 &&
        reg != AGNUS_BPL1MOD &&
        reg != AGNUS_BPL2MOD)
        return 0;

    return 1;
}

int agnus_handles_write(const AgnusState *s, uint32_t addr)
{
    (void)s;

    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;

    uint16_t reg = (uint16_t)(addr & 0x1FEu);

    if (reg == 0x009Au || reg == 0x009Cu)
        return 0;

    if (reg >= 0x0100u &&
        reg != AGNUS_BPLCON0 &&
        reg != AGNUS_BPL1MOD &&
        reg != AGNUS_BPL2MOD)
        return 0;

    return 1;
}

uint32_t agnus_read(AgnusState *s, uint32_t addr, unsigned int size)
{
    uint32_t value = agnus_read_reg(s, (uint16_t)(addr & 0x1FEu));
    if (size == 1)
        return (addr & 1u) ? (value & 0xFFu) : ((value >> 8) & 0xFFu);
    return value;
}

void agnus_write(AgnusState *s, uint32_t addr, uint32_t value, unsigned int size)
{
    agnus_write_reg(s, (uint16_t)(addr & 0x1FEu), value, (int)size);
}

/* ---------------------------------------------------------------------------
 * Low-level register API
 * ------------------------------------------------------------------------- */

uint32_t agnus_read_reg(AgnusState *s, uint16_t reg)
{
    switch (reg)
    {
    case AGNUS_DMACONR:
        return (uint32_t)agnus_dma_read_dmaconr(&s->dma);

    case AGNUS_VPOSR:
    {
        uint16_t v, h;
        agnus_get_beam(s, &v, &h);
        return v;
    }

    case AGNUS_VHPOSR:
    {
        uint16_t v, h;
        agnus_get_beam(s, &v, &h);
        return h;
    }

    case COPPER_COP1LCH:
    case COPPER_COP1LCL:
    case COPPER_COP2LCH:
    case COPPER_COP2LCL:
        return copper_read_reg(&s->copper, reg);

    default:
        if (agnus_is_blitter_reg(reg))
            return blitter_read_reg(&s->blitter, reg);

        return 0;
    }
}

void agnus_write_reg(AgnusState *s, uint16_t reg, uint32_t value, int size)
{
    (void)size;

    uint16_t raw = (uint16_t)value;
    uint32_t pc = bellatrix_debug_cpu_pc();

    if (reg >= AGNUS_BPL1PTH && reg <= AGNUS_BPL6PTL)
    {
        kprintf("[BPL-WRITE] reg=%04x value=%04x pc=%08x\n",
                reg,
                raw,
                pc);
    }

    if (agnus_is_blitter_reg(reg))
    {
        if (reg == AGNUS_BLTSIZE || raw != 0u ||
            (pc >= 0x00FCC980u && pc <= 0x00FCD620u) ||
            (pc >= 0x00FCA480u && pc <= 0x00FCA568u) ||
            (pc >= 0x00FE8800u && pc <= 0x00FE8848u))
        {
            kprintf("[AGNUS-BLT-W] pc=%08x reg=%04x value=%04x\n",
                    (unsigned)pc,
                    (unsigned)reg,
                    (unsigned)raw);
        }
    }

    switch (reg)
    {
    case AGNUS_DMACON:
    {
        uint16_t old = agnus_dmacon_current(s);

        agnus_dma_write_dmacon(&s->dma, raw);
        s->dmacon = s->dma.dmacon;
        if (s->paula)
            paula_audio_set_dmacon(&s->paula->audio, s->dma.dmacon);

        kprintf("[DMACON-W] pc=%08x raw=%04x old=%04x new=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)raw,
                (unsigned)old,
                (unsigned)agnus_dmacon_current(s));
        return;
    }

    /*
     * Copper pointer / strobe / COPCON registers.
     *
     * These are now delegated to copper_regs.c.
     */
    case COPPER_COP1LCH:
    case COPPER_COP1LCL:
    case COPPER_COP2LCH:
    case COPPER_COP2LCL:
    case COPPER_COPJMP1:
    case COPPER_COPJMP2:
    case COPPER_COPINS:
    case AGNUS_COPCON:
        copper_write_reg(&s->copper, reg, raw);
        return;

    case AGNUS_DIWSTRT:
        s->diwstrt = raw;
        return;

    case AGNUS_DIWSTOP:
        s->diwstop = raw;
        return;

    case AGNUS_DDFSTRT:
        s->ddfstrt = raw;
        return;

    case AGNUS_DDFSTOP:
        s->ddfstop = raw;
        return;

    case AGNUS_BPLCON0:
        s->bplcon0 = raw;
        if (s->denise)
            denise_write_reg(s->denise, reg, raw);
        kprintf("[AGNUS] BPLCON0=%04x nplanes=%u hires=%u copper_pc=%05x vh=%04x\n",
                (unsigned)raw,
                (unsigned)((raw >> 12) & 7u),
                (unsigned)((raw >> 15) & 1u),
                (unsigned)(s->copper.pc & CHIP_RAM_WORD_MASK),
                (unsigned)(((s->beam.vpos & 0xFFu) << 8) | (s->beam.hpos & 0xFEu)));
        return;

    case AGNUS_BPL1PTH:
        s->bplpth[0] = raw;
        kprintf("[AGNUS] BPL1PTH=%04x ptr=%05x\n",
                (unsigned)raw,
                (unsigned)agnus_bpl_ptr(s, 0));
        return;

    case AGNUS_BPL1PTL:
        s->bplptl[0] = raw;
        kprintf("[AGNUS] BPL1PTL=%04x ptr=%05x\n",
                (unsigned)raw,
                (unsigned)agnus_bpl_ptr(s, 0));
        return;

    case AGNUS_BPL2PTH:
        s->bplpth[1] = raw;
        kprintf("[AGNUS] BPL2PTH=%04x ptr=%05x\n",
                (unsigned)raw,
                (unsigned)((((uint32_t)(raw & 0x1Fu)) << 16) |
                           s->bplptl[1]));
        return;

    case AGNUS_BPL2PTL:
        s->bplptl[1] = raw;
        kprintf("[AGNUS] BPL2PTL=%04x ptr=%05x\n",
                (unsigned)raw,
                (unsigned)((((uint32_t)(s->bplpth[1] & 0x1Fu)) << 16) |
                           (raw & 0xFFFEu)));
        return;

    case AGNUS_BPL3PTH:
        s->bplpth[2] = raw;
        kprintf("[AGNUS] BPL3PTH=%04x\n", (unsigned)raw);
        return;

    case AGNUS_BPL3PTL:
        s->bplptl[2] = raw;
        kprintf("[AGNUS] BPL3PTL=%04x\n", (unsigned)raw);
        return;

    case AGNUS_BPL4PTH:
        s->bplpth[3] = raw;
        kprintf("[AGNUS] BPL4PTH=%04x\n", (unsigned)raw);
        return;

    case AGNUS_BPL4PTL:
        s->bplptl[3] = raw;
        kprintf("[AGNUS] BPL4PTL=%04x\n", (unsigned)raw);
        return;

    case AGNUS_BPL5PTH:
        s->bplpth[4] = raw;
        kprintf("[AGNUS] BPL5PTH=%04x\n", (unsigned)raw);
        return;

    case AGNUS_BPL5PTL:
        s->bplptl[4] = raw;
        kprintf("[AGNUS] BPL5PTL=%04x\n", (unsigned)raw);
        return;

    case AGNUS_BPL6PTH:
        s->bplpth[5] = raw;
        kprintf("[AGNUS] BPL6PTH=%04x\n", (unsigned)raw);
        return;

    case AGNUS_BPL6PTL:
        s->bplptl[5] = raw;
        kprintf("[AGNUS] BPL6PTL=%04x\n", (unsigned)raw);
        return;

    case AGNUS_BPL1MOD:
        s->bpl1mod = (int16_t)raw;
        return;

    case AGNUS_BPL2MOD:
        s->bpl2mod = (int16_t)raw;
        return;

    default:
        break;
    }

    /*
     * Paula-owned interrupt registers.
     */
    if (reg == 0x009Au || reg == 0x009Cu)
    {
        if (s->paula)
            paula_write(s->paula, 0xDFF000u | reg, raw, 2);

        return;
    }

    /*
     * Blitter registers.
     */
    if (agnus_is_blitter_reg(reg))
    {
        blitter_write_reg(&s->blitter, s, reg, raw);
        return;
    }

    /*
     * Denise/custom display registers.
     */
    if (reg >= 0x0100u &&
        reg != AGNUS_BPL1MOD &&
        reg != AGNUS_BPL2MOD)
    {
        if (reg == AGNUS_BPLCON0)
            s->bplcon0 = raw;

        if (s->denise)
            denise_write_reg(s->denise, reg, raw);

        return;
    }

    (void)raw;
}
