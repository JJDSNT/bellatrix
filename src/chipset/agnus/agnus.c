// src/chipset/agnus/agnus.c

#include "agnus.h"
#include "chipset/denise/denise.h"
#include "host/pal.h"
#include "support.h"

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static inline void agnus_apply_setclr_15(uint16_t *dst, uint16_t raw, uint16_t writable_mask)
{
    uint16_t bits = (uint16_t)(raw & writable_mask);

    if (raw & 0x8000u) {
        *dst |= bits;
    } else {
        *dst &= (uint16_t)~bits;
    }
}

static inline void agnus_notify_ipl(const AgnusState *s)
{
    uint8_t ipl = agnus_compute_ipl(s);

    if (ipl) {
        PAL_IPL_Set(ipl);
    } else {
        PAL_IPL_Clear();
    }
}

static inline void agnus_get_beam(const AgnusState *s, uint16_t *vposr_out, uint16_t *vhposr_out)
{
    *vposr_out  = (s->vpos >= 256u) ? 1u : 0u;
    *vhposr_out = (uint16_t)(((s->vpos & 0xFFu) << 8) | (s->hpos & 0xFFu));
}

static inline int agnus_is_blitter_reg(uint16_t reg)
{
    switch (reg) {
    case AGNUS_BLTCON0:
    case AGNUS_BLTCON1:
    case AGNUS_BLTCPTH:
    case AGNUS_BLTCPTL:
    case AGNUS_BLTBPTH:
    case AGNUS_BLTBPTL:
    case AGNUS_BLTAPTH:
    case AGNUS_BLTAPTL:
    case AGNUS_BLTDPTH:
    case AGNUS_BLTDPTL:
    case AGNUS_BLTSIZE:
    case AGNUS_BLTCMOD:
    case AGNUS_BLTBMOD:
    case AGNUS_BLTAMOD:
    case AGNUS_BLTDMOD:
        return 1;
    default:
        return 0;
    }
}

static inline int agnus_is_copper_reg(uint16_t reg)
{
    switch (reg) {
    case AGNUS_COP1LCH:
    case AGNUS_COP1LCL:
    case AGNUS_COP2LCH:
    case AGNUS_COP2LCL:
    case AGNUS_COPJMP1:
    case AGNUS_COPJMP2:
    case AGNUS_COPINS:
        return 1;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void agnus_init(AgnusState *s)
{
    s->intena    = 0;
    s->intreq    = 0;
    s->dmacon    = 0;
    s->hpos      = 0;
    s->vpos      = 0;
    s->vbl_count = 0;

    s->diwstrt = 0x2C81;  // PAL default: line 44, hpos 0x81
    s->diwstop = 0xF4C1;  // PAL default: line 300 (0xF4 wraps), hpos 0xC1
    s->ddfstrt = 0x0038;
    s->ddfstop = 0x00D0;

    for (int i = 0; i < 6; i++) {
        s->bplpth[i] = 0;
        s->bplptl[i] = 0;
    }

    blitter_init(&s->blitter);
    copper_init(&s->copper);
}

// ---------------------------------------------------------------------------
// IPL
// ---------------------------------------------------------------------------

uint8_t agnus_compute_ipl(const AgnusState *s)
{
    int master = !!(s->intena & INT_INTEN);
    uint16_t pending = (uint16_t)(s->intena & s->intreq & 0x3FFFu);

    if (!master || !pending) {
        return 0;
    }

    if (pending & INT_EXTER)                                    return 6;
    if (pending & INT_RBF)                                      return 5;
    if (pending & (INT_AUD0 | INT_AUD1 | INT_AUD2 | INT_AUD3)) return 4;
    if (pending & INT_VERTB)                                    return 3;
    if (pending & INT_PORTS)                                    return 2;
    if (pending & (INT_TBE | INT_DSKBLK | INT_SOFTINT))         return 1;

    return 0;
}

// ---------------------------------------------------------------------------
// INTREQ
// ---------------------------------------------------------------------------

void agnus_intreq_set(AgnusState *s, uint16_t bits)
{
    s->intreq |= (uint16_t)(bits & 0x3FFFu);
    agnus_notify_ipl(s);
}

void agnus_intreq_clear(AgnusState *s, uint16_t bits)
{
    s->intreq &= (uint16_t)~(bits & 0x3FFFu);
    agnus_notify_ipl(s);
}

// ---------------------------------------------------------------------------
// Busy state
// ---------------------------------------------------------------------------

int agnus_blitter_busy(const AgnusState *s)
{
    if (!blitter_is_busy(&s->blitter)) {
        return 0;
    }

    if (!(s->dmacon & DMAF_DMAEN)) {
        return 0;
    }

    if (!(s->dmacon & DMAF_BLTEN)) {
        return 0;
    }

    return 1;
}

// ---------------------------------------------------------------------------
// Step
// ---------------------------------------------------------------------------

void agnus_step(AgnusState *s, uint64_t ticks)
{
    if (!ticks) {
        return;
    }

    // Blitter progresses with Agnus time.
    blitter_step(&s->blitter, s, ticks);

    // Simplified PAL beam progression.
    uint64_t total_h = (uint64_t)s->hpos + ticks;
    uint64_t lines_advanced = total_h / AGNUS_PAL_HPOS;
    s->hpos = (uint32_t)(total_h % AGNUS_PAL_HPOS);

    uint64_t total_v = (uint64_t)s->vpos + lines_advanced;
    uint32_t frames = (uint32_t)(total_v / AGNUS_PAL_LINES);
    s->vpos = (uint32_t)(total_v % AGNUS_PAL_LINES);

    while (frames--) {
        s->vbl_count++;

        kprintf("[VBL] frame=%u hpos=%u vpos=%u intena=0x%04x intreq=0x%04x dmacon=0x%04x\n",
                (unsigned)s->vbl_count,
                (unsigned)s->hpos,
                (unsigned)s->vpos,
                (unsigned)s->intena,
                (unsigned)s->intreq,
                (unsigned)s->dmacon);

        // Raise VERTB
        agnus_intreq_set(s, INT_VERTB);

        // Copper runs first — sets up BPLxPT, COLOR, BPLCON for this frame.
        copper_vbl_execute(&s->copper, s);

        // Render the frame into the Emu68 framebuffer.
        denise_render_frame(s);
    }

    // Future:
    // copper_step_scanline(...)
    // DMA scheduler
    // audio timing hooks
}

// ---------------------------------------------------------------------------
// MMIO
// ---------------------------------------------------------------------------

uint32_t agnus_read_reg(AgnusState *s, uint16_t reg)
{
    switch (reg) {
    case AGNUS_DMACONR:
        return s->dmacon;

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

    case AGNUS_INTENAR:
        return s->intena;

    case AGNUS_INTREQR:
        return s->intreq;

    default:
        return 0;
    }
}

void agnus_write_reg(AgnusState *s, uint16_t reg, uint32_t value, int size)
{
    (void)size;

    uint16_t raw = (uint16_t)value;

    switch (reg) {
    case AGNUS_INTENA:
        agnus_apply_setclr_15(&s->intena, raw, 0x7FFFu);
        agnus_notify_ipl(s);
        return;

    case AGNUS_INTREQ:
        agnus_apply_setclr_15(&s->intreq, raw, 0x3FFFu);
        agnus_notify_ipl(s);
        return;

    case AGNUS_DMACON:
        agnus_apply_setclr_15(&s->dmacon, raw, 0x7FFFu);
        return;

    // Display window / data fetch
    case AGNUS_DIWSTRT: s->diwstrt = raw; return;
    case AGNUS_DIWSTOP: s->diwstop = raw; return;
    case AGNUS_DDFSTRT: s->ddfstrt = raw; return;
    case AGNUS_DDFSTOP: s->ddfstop = raw; return;

    // Bitplane pointers
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

    // Denise-owned: BPLCON, BPL1/2MOD, COLOR palette
    if (reg == 0x0100u || reg == 0x0102u || reg == 0x0104u ||
        reg == 0x0108u || reg == 0x010Au ||
        (reg >= 0x0180u && reg <= 0x01BEu && (reg & 1) == 0)) {
        denise_write(reg, raw);
        return;
    }

    (void)raw;
}