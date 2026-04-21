// src/chipset/agnus/agnus.c

#include "agnus.h"
#include "chipset/denise/denise.h"
#include "chipset/paula/paula.h"
#include "host/pal.h"
#include "support.h"

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static inline void agnus_apply_setclr_15(uint16_t *dst, uint16_t raw, uint16_t writable_mask)
{
    uint16_t bits = (uint16_t)(raw & writable_mask);
    if (raw & 0x8000u)
        *dst |= bits;
    else
        *dst &= (uint16_t)~bits;
}

static inline void agnus_get_beam(const AgnusState *s,
                                  uint16_t *vposr_out,
                                  uint16_t *vhposr_out)
{
    *vposr_out = (s->beam.vpos >= 256u) ? 1u : 0u;
    *vhposr_out = (uint16_t)(((s->beam.vpos & 0xFFu) << 8) | (s->beam.hpos & 0xFFu));
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void agnus_init(AgnusState *s)
{
    s->dmacon = 0;

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

    blitter_init(&s->blitter);
    copper_init(&s->copper);

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
    if (!blitter_is_busy(&s->blitter))
        return 0;
    if (!(s->dmacon & DMAF_DMAEN))
        return 0;
    if (!(s->dmacon & DMAF_BLTEN))
        return 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Step
 * ------------------------------------------------------------------------- */

void agnus_step(AgnusState *s, uint64_t ticks)
{
    if (!ticks)
        return;

    /*
     * Step subcomponents that consume elapsed time directly.
     */
    blitter_step(&s->blitter, s, ticks);

    /*
     * Advance beam first, because Copper timing and VBL edge detection
     * depend on current raster position.
     */
    beam_step(&s->beam, ticks);

    /*
     * Copper now runs as a state machine driven by beam position.
     * Do not batch-execute the list at VBL anymore.
     *
     * For now, we give it one execution slice per agnus_step call.
     * If later needed, this can be scaled to multiple micro-steps.
     */
    copper_step(&s->copper, s);

    /*
     * Check for vertical blank entry — fires once per VBL transition.
     */
    if (beam_vblank_entered(&s->beam))
    {
        kprintf("[VBL-ENTER] frame=%u hpos=%u vpos=%u dmacon=0x%04x intena=0x%04x intreq=0x%04x\n",
                (unsigned)s->beam.frame,
                (unsigned)s->beam.hpos,
                (unsigned)s->beam.vpos,
                (unsigned)s->dmacon,
                s->paula ? (unsigned)s->paula->intena : 0u,
                s->paula ? (unsigned)s->paula->intreq : 0u);

        /*
         * Paula VERTB interrupt.
         */
        agnus_intreq_set(s, PAULA_INT_VERTB);

        /*
         * Reload Copper from COP1LC at VBL.
         * This replaces the old copper_vbl_execute() batch model.
         */
        copper_vbl_reload(&s->copper);

        /*
         * Keep current simplified frame render path for now.
         * Denise still renders a full frame snapshot at VBL in this phase.
         *
         * Later, once bitplane fetch is fully beam-driven, this block should
         * move out of VBL and become scanline/raster driven too.
         */
        if (s->denise && s->memory)
        {
            int nplanes = (s->denise->bplcon0 >> 12) & 7;
            if (nplanes > 6)
                nplanes = 6;

            {
                int hires = (s->denise->bplcon0 >> 15) & 1;

                int vstart = (s->diwstrt >> 8) & 0xFF;
                int vstop = (s->diwstop >> 8) & 0xFF;
                if (vstop <= vstart)
                    vstop += 256;

                {
                    int vheight = vstop - vstart;
                    if (vheight <= 0)
                        vheight = 1;
                    if (vheight > 512)
                        vheight = 512;

                    kprintf("[DENISE] render frame=%u nplanes=%d hires=%d vheight=%d"
                            " bpt0=0x%05x dmacon=0x%04x\n",
                            (unsigned)s->beam.frame,
                            nplanes,
                            hires,
                            vheight,
                            (unsigned)((((uint32_t)(s->bplpth[0] & 0x1Fu)) << 16) |
                                       ((uint32_t)(s->bplptl[0] & 0xFFFEu))),
                            (unsigned)s->dmacon);

                    bitplanes_begin_frame(&s->bitplanes, s, nplanes, hires);

                    for (int line = 0; line < vheight; ++line)
                    {
                        bitplanes_fetch_line(&s->bitplanes, s, vstart + line);

                        if (bitplanes_line_ready(&s->bitplanes))
                        {
                            denise_render_line(s->denise, &s->bitplanes, line, vheight);
                            bitplanes_clear_line_ready(&s->bitplanes);
                        }
                    }
                }
            }
        }
    }

    /*
     * Track VBL exit so the next entry edge can be detected.
     */
    beam_vblank_exited(&s->beam);
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
    /* Paula owns INTENAR (0x1C) and INTREQR (0x1E) */
    if (reg == 0x001Cu || reg == 0x001Eu)
        return 0;
    /* Denise owns colour/bitplane control registers (>= 0x100),
     * except BPL1MOD/BPL2MOD which are Agnus-owned DMA registers */
    if (reg >= 0x0100u && reg != AGNUS_BPL1MOD && reg != AGNUS_BPL2MOD)
        return 0;
    return 1;
}

int agnus_handles_write(const AgnusState *s, uint32_t addr)
{
    (void)s;
    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;
    uint16_t reg = (uint16_t)(addr & 0x1FEu);
    /* Paula owns INTENA (0x9A) and INTREQ (0x9C) */
    if (reg == 0x009Au || reg == 0x009Cu)
        return 0;
    /* Denise owns colour/bitplane control registers (>= 0x100),
     * except BPL1MOD/BPL2MOD which are Agnus-owned DMA registers */
    if (reg >= 0x0100u && reg != AGNUS_BPL1MOD && reg != AGNUS_BPL2MOD)
        return 0;
    return 1;
}

uint32_t agnus_read(AgnusState *s, uint32_t addr, unsigned int size)
{
    (void)size;
    return agnus_read_reg(s, (uint16_t)(addr & 0x1FEu));
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
        return (uint32_t)(s->dmacon | (uint16_t)(blitter_is_busy(&s->blitter) ? AGNUS_DMACON_BBUSY : 0u) | (uint16_t)(s->blitter.zero ? AGNUS_DMACON_BZERO : 0u));

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

    switch (reg)
    {
    case AGNUS_DMACON:
        agnus_apply_setclr_15(&s->dmacon, raw, 0x7FFFu);
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

    case AGNUS_BPL1PTH:
        s->bplpth[0] = raw;
        return;
    case AGNUS_BPL1PTL:
        s->bplptl[0] = raw;
        return;
    case AGNUS_BPL2PTH:
        s->bplpth[1] = raw;
        return;
    case AGNUS_BPL2PTL:
        s->bplptl[1] = raw;
        return;
    case AGNUS_BPL3PTH:
        s->bplpth[2] = raw;
        return;
    case AGNUS_BPL3PTL:
        s->bplptl[2] = raw;
        return;
    case AGNUS_BPL4PTH:
        s->bplpth[3] = raw;
        return;
    case AGNUS_BPL4PTL:
        s->bplptl[3] = raw;
        return;
    case AGNUS_BPL5PTH:
        s->bplpth[4] = raw;
        return;
    case AGNUS_BPL5PTL:
        s->bplptl[4] = raw;
        return;
    case AGNUS_BPL6PTH:
        s->bplpth[5] = raw;
        return;
    case AGNUS_BPL6PTL:
        s->bplptl[5] = raw;
        return;

    /* BPL modulos — Agnus-owned DMA registers */
    case AGNUS_BPL1MOD:
        s->bpl1mod = (int16_t)raw;
        return;
    case AGNUS_BPL2MOD:
        s->bpl2mod = (int16_t)raw;
        return;

    default:
        break;
    }

    /* INTENA/INTREQ — forwarded to Paula (Copper may write these directly) */
    if (reg == 0x009Au || reg == 0x009Cu)
    {
        if (s->paula)
            paula_write(s->paula, 0xDFF000u | reg, raw, 2);
        return;
    }

    if (agnus_is_blitter_reg(reg))
    {
        blitter_write_reg(&s->blitter, s, reg, raw);
        return;
    }

    if (agnus_is_copper_reg(reg))
    {
        copper_write_reg(&s->copper, reg, raw);
        return;
    }

    /* Denise-owned registers — forward via attached pointer */
    if (reg >= 0x0100u &&
        reg != AGNUS_BPL1MOD && reg != AGNUS_BPL2MOD)
    {
        if (s->denise)
            denise_write_reg(s->denise, reg, raw);
        return;
    }

    (void)raw;
}
