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
    *vposr_out  = (s->vpos >= 256u) ? 1u : 0u;
    *vhposr_out = (uint16_t)(((s->vpos & 0xFFu) << 8) | (s->hpos & 0xFFu));
}

static inline int agnus_is_blitter_reg(uint16_t reg)
{
    switch (reg) {
    case AGNUS_BLTCON0:
    case AGNUS_BLTCON1:
    case AGNUS_BLTCPTH: case AGNUS_BLTCPTL:
    case AGNUS_BLTBPTH: case AGNUS_BLTBPTL:
    case AGNUS_BLTAPTH: case AGNUS_BLTAPTL:
    case AGNUS_BLTDPTH: case AGNUS_BLTDPTL:
    case AGNUS_BLTSIZE:
    case AGNUS_BLTCMOD: case AGNUS_BLTBMOD:
    case AGNUS_BLTAMOD: case AGNUS_BLTDMOD:
        return 1;
    default:
        return 0;
    }
}

static inline int agnus_is_copper_reg(uint16_t reg)
{
    switch (reg) {
    case AGNUS_COP1LCH: case AGNUS_COP1LCL:
    case AGNUS_COP2LCH: case AGNUS_COP2LCL:
    case AGNUS_COPJMP1: case AGNUS_COPJMP2:
    case AGNUS_COPINS:
        return 1;
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void agnus_init(AgnusState *s)
{
    s->dmacon    = 0;
    s->hpos      = 0;
    s->vpos      = 0;
    s->vbl_count = 0;

    s->diwstrt = 0x2C81u;
    s->diwstop = 0xF4C1u;
    s->ddfstrt = 0x0038u;
    s->ddfstop = 0x00D0u;

    for (int i = 0; i < 6; i++) {
        s->bplpth[i] = 0;
        s->bplptl[i] = 0;
    }

    blitter_init(&s->blitter);
    copper_init(&s->copper);

    s->denise = NULL;
    s->paula  = NULL;
}

void agnus_reset(AgnusState *s)
{
    struct Denise *saved_denise = s->denise;
    struct Paula  *saved_paula  = s->paula;

    agnus_init(s);

    s->denise = saved_denise;
    s->paula  = saved_paula;
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

    blitter_step(&s->blitter, s, ticks);

    uint64_t total_h    = (uint64_t)s->hpos + ticks;
    uint64_t lines_adv  = total_h / AGNUS_PAL_HPOS;
    s->hpos             = (uint32_t)(total_h % AGNUS_PAL_HPOS);

    uint64_t total_v    = (uint64_t)s->vpos + lines_adv;
    uint32_t frames     = (uint32_t)(total_v / AGNUS_PAL_LINES);
    s->vpos             = (uint32_t)(total_v % AGNUS_PAL_LINES);

    while (frames--) {
        s->vbl_count++;

        kprintf("[VBL] frame=%u hpos=%u vpos=%u dmacon=0x%04x\n",
                (unsigned)s->vbl_count,
                (unsigned)s->hpos,
                (unsigned)s->vpos,
                (unsigned)s->dmacon);

        agnus_intreq_set(s, PAULA_INT_VERTB);

        copper_vbl_execute(&s->copper, s);

        if (s->denise)
            denise_render_frame(s->denise, s);
    }
}

/* ---------------------------------------------------------------------------
 * Bus protocol
 * ------------------------------------------------------------------------- */

int agnus_handles_read(const AgnusState *s, uint32_t addr)
{
    (void)s;
    /* Agnus custom registers: 0xDFF000-0xDFF1FF, excluding Paula-owned addresses */
    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;
    uint16_t reg = (uint16_t)(addr & 0x1FEu);
    /* Paula owns INTENAR (0x1C) and INTREQR (0x1E) */
    if (reg == 0x001Cu || reg == 0x001Eu)
        return 0;
    /* Denise owns colour/bitplane control registers */
    if (reg >= 0x0100u)
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
    /* Denise owns colour/bitplane control registers (>= 0x100) */
    if (reg >= 0x0100u)
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
    switch (reg) {
    case AGNUS_DMACONR: return s->dmacon;

    case AGNUS_VPOSR: {
        uint16_t v, h;
        agnus_get_beam(s, &v, &h);
        return v;
    }

    case AGNUS_VHPOSR: {
        uint16_t v, h;
        agnus_get_beam(s, &v, &h);
        return h;
    }

    default:
        return 0;
    }
}

void agnus_write_reg(AgnusState *s, uint16_t reg, uint32_t value, int size)
{
    (void)size;
    uint16_t raw = (uint16_t)value;

    switch (reg) {
    case AGNUS_DMACON:
        agnus_apply_setclr_15(&s->dmacon, raw, 0x7FFFu);
        return;

    case AGNUS_DIWSTRT: s->diwstrt = raw; return;
    case AGNUS_DIWSTOP: s->diwstop = raw; return;
    case AGNUS_DDFSTRT: s->ddfstrt = raw; return;
    case AGNUS_DDFSTOP: s->ddfstop = raw; return;

    case AGNUS_BPL1PTH: s->bplpth[0] = raw; return;
    case AGNUS_BPL1PTL: s->bplptl[0] = raw; return;
    case AGNUS_BPL2PTH: s->bplpth[1] = raw; return;
    case AGNUS_BPL2PTL: s->bplptl[1] = raw; return;
    case AGNUS_BPL3PTH: s->bplpth[2] = raw; return;
    case AGNUS_BPL3PTL: s->bplptl[2] = raw; return;
    case AGNUS_BPL4PTH: s->bplpth[3] = raw; return;
    case AGNUS_BPL4PTL: s->bplptl[3] = raw; return;
    case AGNUS_BPL5PTH: s->bplpth[4] = raw; return;
    case AGNUS_BPL5PTL: s->bplptl[4] = raw; return;
    case AGNUS_BPL6PTH: s->bplpth[5] = raw; return;
    case AGNUS_BPL6PTL: s->bplptl[5] = raw; return;

    default:
        break;
    }

    if (agnus_is_blitter_reg(reg)) {
        blitter_write_reg(&s->blitter, s, reg, raw);
        return;
    }

    if (agnus_is_copper_reg(reg)) {
        copper_write_reg(&s->copper, reg, raw);
        return;
    }

    /* Denise-owned registers — forward via attached pointer */
    if (reg == 0x0100u || reg == 0x0102u || reg == 0x0104u ||
        reg == 0x0108u || reg == 0x010Au ||
        (reg >= 0x0180u && reg <= 0x01BEu && (reg & 1u) == 0u)) {
        if (s->denise)
            denise_write_reg(s->denise, reg, raw);
        return;
    }

    /* INTENA / INTREQ — forward to Paula */
    if (reg == 0x009Au || reg == 0x009Cu) {
        uint32_t full_addr = 0xDFF000u | reg;
        if (s->paula)
            paula_write(s->paula, full_addr, value, (unsigned)size);
        return;
    }

    (void)raw;
}
