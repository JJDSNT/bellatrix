#include "blitter.h"

#include <string.h>

#include "memory/memory.h"
#include "agnus.h"
#include "chipset/paula/paula.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

#define BLTCON0_ASH_MASK   0xF000u
#define BLTCON1_BSH_MASK   0xF000u

#define BLTCON0_USEA       0x0800u
#define BLTCON0_USEB       0x0400u
#define BLTCON0_USEC       0x0200u
#define BLTCON0_USED       0x0100u

#define BLTCON1_LINE       0x0001u
#define BLTCON1_DESC       0x0002u

static inline uint32_t make_ptr(uint16_t hi, uint16_t lo)
{
    return ((uint32_t)hi << 16) | (uint32_t)lo;
}

static inline uint16_t ptr_hi(uint32_t ptr)
{
    return (uint16_t)(ptr >> 16);
}

static inline uint16_t ptr_lo(uint32_t ptr)
{
    return (uint16_t)(ptr & 0xffffu);
}

static inline uint16_t blitter_width_words(const BlitterState *b)
{
    uint16_t width = b->bltsize & 0x003fu;
    return width ? width : 64u;
}

static inline uint16_t blitter_height_rows(const BlitterState *b)
{
    uint16_t height = (b->bltsize >> 6) & 0x03ffu;
    return height ? height : 1024u;
}

static inline uint32_t blitter_compute_cycles(uint16_t bltsize)
{
    uint16_t width  = bltsize & 0x003fu;
    uint16_t height = (bltsize >> 6) & 0x03ffu;

    if (width == 0)
        width = 64u;

    if (height == 0)
        height = 1024u;

    return (uint32_t)width * (uint32_t)height;
}

static inline void blitter_set_busy(BlitterState *b, AgnusState *agnus, int busy)
{
    (void)agnus;
    b->busy = busy ? 1 : 0;
}

static inline void blitter_set_zero(BlitterState *b, AgnusState *agnus, int zero)
{
    (void)agnus;
    b->zero = zero ? 1 : 0;
}

static inline uint16_t blitter_ash(const BlitterState *b)
{
    return (uint16_t)((b->bltcon0 & BLTCON0_ASH_MASK) >> 12);
}

static inline uint16_t blitter_bsh(const BlitterState *b)
{
    return (uint16_t)((b->bltcon1 & BLTCON1_BSH_MASK) >> 12);
}

static inline int blitter_use_a(const BlitterState *b)
{
    return (b->bltcon0 & BLTCON0_USEA) ? 1 : 0;
}

static inline int blitter_use_b(const BlitterState *b)
{
    return (b->bltcon0 & BLTCON0_USEB) ? 1 : 0;
}

static inline int blitter_use_c(const BlitterState *b)
{
    return (b->bltcon0 & BLTCON0_USEC) ? 1 : 0;
}

static inline int blitter_use_d(const BlitterState *b)
{
    return (b->bltcon0 & BLTCON0_USED) ? 1 : 0;
}

static inline int blitter_desc(const BlitterState *b)
{
    return (b->bltcon1 & BLTCON1_DESC) ? 1 : 0;
}

static inline int blitter_line_mode(const BlitterState *b)
{
    return (b->bltcon1 & BLTCON1_LINE) ? 1 : 0;
}

static inline uint16_t blitter_chip_read16(AgnusState *agnus, uint32_t addr)
{
    if (!agnus || !agnus->memory)
        return 0xFFFFu;

    return bellatrix_chip_read16(agnus->memory, addr);
}

static inline void blitter_chip_write16(AgnusState *agnus, uint32_t addr, uint16_t value)
{
    if (!agnus || !agnus->memory)
        return;

    bellatrix_chip_write16(agnus->memory, addr, value);
}

static inline uint16_t blitter_barrel_shift(uint16_t anew, uint16_t aold,
                                            uint16_t shift, int desc)
{
    uint32_t pair;

    if (shift == 0)
        return anew;

    if (desc) {
        pair = ((uint32_t)anew << 16) | (uint32_t)aold;
        return (uint16_t)(pair >> (16u - shift));
    } else {
        pair = ((uint32_t)aold << 16) | (uint32_t)anew;
        return (uint16_t)(pair >> shift);
    }
}

static inline uint16_t blitter_do_minterm(uint16_t a, uint16_t b,
                                          uint16_t c, uint8_t minterm)
{
    uint16_t result = 0;

    if (minterm & 0x80u) result |=  (uint16_t)( a &  b &  c);
    if (minterm & 0x40u) result |=  (uint16_t)( a &  b & ~c);
    if (minterm & 0x20u) result |=  (uint16_t)( a & ~b &  c);
    if (minterm & 0x10u) result |=  (uint16_t)( a & ~b & ~c);
    if (minterm & 0x08u) result |=  (uint16_t)(~a &  b &  c);
    if (minterm & 0x04u) result |=  (uint16_t)(~a &  b & ~c);
    if (minterm & 0x02u) result |=  (uint16_t)(~a & ~b &  c);
    if (minterm & 0x01u) result |=  (uint16_t)(~a & ~b & ~c);

    return result;
}

static uint16_t blitter_fill_word(uint16_t d, uint8_t *carry,
                                  int inclusive, int desc)
{
    uint16_t result = 0;
    uint8_t c = *carry;

    if (!desc)
    {
        for (int i = 15; i >= 0; --i)
        {
            uint8_t bit = (uint8_t)((d >> i) & 1u);

            if (inclusive)
            {
                if (bit)
                    c ^= 1u;
                if (c)
                    result |= (uint16_t)(1u << i);
            }
            else
            {
                if ((c ^ bit) != 0)
                    result |= (uint16_t)(1u << i);
                if (bit)
                    c ^= 1u;
            }
        }
    }
    else
    {
        for (int i = 0; i <= 15; ++i)
        {
            uint8_t bit = (uint8_t)((d >> i) & 1u);

            if (inclusive)
            {
                if (bit)
                    c ^= 1u;
                if (c)
                    result |= (uint16_t)(1u << i);
            }
            else
            {
                if ((c ^ bit) != 0)
                    result |= (uint16_t)(1u << i);
                if (bit)
                    c ^= 1u;
            }
        }
    }

    *carry = c;
    return result;
}

static void blitter_execute_line_fallback(BlitterState *b, AgnusState *agnus)
{
    const uint16_t height_rows = blitter_height_rows(b);
    const unsigned int oct = (unsigned int)((b->bltcon1 >> 2) & 0x7u);
    int16_t error = (int16_t)(b->bltapt & 0xFFFFu);
    int start_pixel = (int)blitter_ash(b);
    int pattern_shift = (int)blitter_bsh(b);
    uint16_t pattern = b->bltbdat;
    uint8_t minterm = (uint8_t)(b->bltcon0 & 0x00FFu);
    uint32_t plane_addr = b->bltcpt & 0x001FFFFEu;
    uint32_t last_addr = plane_addr;
    int plane_mod = (int)b->bltcmod;
    int d = 0;
    int any_nonzero = 0;

    if (height_rows == 0)
    {
        blitter_set_zero(b, agnus, 1);
        return;
    }

    if (pattern_shift != 0)
        pattern = (uint16_t)((pattern >> pattern_shift) |
                             (pattern << (16 - pattern_shift)));

    for (uint16_t i = 0; i < height_rows; ++i)
    {
        int offset;
        uint32_t addr = plane_addr;
        uint16_t pixel;
        uint16_t bitmask;
        uint16_t dval;

        switch (oct)
        {
        case 0:
            offset = d + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) + (uint32_t)(i * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 1:
            offset = d + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) - (uint32_t)(i * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 2:
            offset = d + (15 - start_pixel);
            addr = (plane_addr + 1u) - (uint32_t)(offset >> 3) + (uint32_t)(i * plane_mod);
            bitmask = (uint16_t)(0x0001u << (offset & 15));
            break;

        case 3:
            offset = d + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) - (uint32_t)(i * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 4:
            offset = (int)i + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) + (uint32_t)(d * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 5:
            offset = (int)i + (15 - start_pixel);
            addr = (plane_addr + 1u) - (uint32_t)(offset >> 3) + (uint32_t)(d * plane_mod);
            bitmask = (uint16_t)(0x0001u << (offset & 15));
            break;

        case 6:
            offset = (int)i + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) - (uint32_t)(d * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 7:
        default:
            offset = (int)i + (15 - start_pixel);
            addr = (plane_addr + 1u) - (uint32_t)(offset >> 3) - (uint32_t)(d * plane_mod);
            bitmask = (uint16_t)(0x0001u << (offset & 15));
            break;
        }

        pixel = blitter_chip_read16(agnus, addr);
        dval = blitter_do_minterm(bitmask, pattern, pixel, minterm);

        blitter_chip_write16(agnus, addr, dval);
        b->bltddat = dval;

        if (dval != 0)
            any_nonzero = 1;

        last_addr = addr;

        if (error > 0)
        {
            error = (int16_t)(error + b->bltamod);

            if (oct == 3)
                d -= 1;
            else
                d += 1;
        }
        else
        {
            error = (int16_t)(error + b->bltbmod);
        }
    }

    blitter_set_zero(b, agnus, any_nonzero ? 0 : 1);
    b->bltapt = (b->bltapt & 0xFFFF0000u) | (uint16_t)error;
    b->bltcpt = last_addr;
    b->bltdpt = last_addr;
}

static void blitter_execute_copy(BlitterState *b, AgnusState *agnus)
{
    const int desc = blitter_desc(b);
    const int xinc = desc ? -2 : 2;

    const int useA = blitter_use_a(b);
    const int useB = blitter_use_b(b);
    const int useC = blitter_use_c(b);
    const int useD = blitter_use_d(b);

    const uint16_t ash = blitter_ash(b);
    const uint16_t bsh = blitter_bsh(b);
    const uint8_t  minterm = (uint8_t)(b->bltcon0 & 0x00ffu);
    const int ife = (b->bltcon1 & 0x0008u) ? 1 : 0;
    const int efe = (b->bltcon1 & 0x0010u) ? 1 : 0;
    const uint8_t fci = (b->bltcon1 & 0x0004u) ? 1u : 0u;

    const uint16_t width_words = blitter_width_words(b);
    const uint16_t height_rows = blitter_height_rows(b);

    uint32_t apt = b->bltapt;
    uint32_t bpt = b->bltbpt;
    uint32_t cpt = b->bltcpt;
    uint32_t dpt = b->bltdpt;

    uint16_t aold = b->bltadat;
    uint16_t bold = b->bltbdat;

    int all_zero = 1;

    for (uint16_t y = 0; y < height_rows; ++y) {
        uint8_t fill_carry = fci;

        for (uint16_t x = 0; x < width_words; ++x) {

            uint16_t araw = b->bltadat;
            uint16_t braw = b->bltbdat;
            uint16_t cval = b->bltcdat;
            uint16_t aval;
            uint16_t bval;
            uint16_t dval;

            if (useA) {
                uint16_t mask = 0xFFFFu;

                araw = blitter_chip_read16(agnus, apt);
                apt = (uint32_t)(apt + xinc);

                if (x == 0)
                    mask &= b->bltafwm;
                if (x == (uint16_t)(width_words - 1))
                    mask &= b->bltalwm;

                araw &= mask;
                b->bltadat = araw;

                aval = blitter_barrel_shift(araw, aold, ash, desc);
                aold = araw;
            } else {
                aval = b->bltadat;
            }

            if (useB) {
                braw = blitter_chip_read16(agnus, bpt);
                bpt = (uint32_t)(bpt + xinc);

                b->bltbdat = braw;
                bval = blitter_barrel_shift(braw, bold, bsh, desc);
                bold = braw;
            } else {
                bval = b->bltbdat;
            }

            if (useC) {
                cval = blitter_chip_read16(agnus, cpt);
                cpt = (uint32_t)(cpt + xinc);
                b->bltcdat = cval;
            }

            dval = blitter_do_minterm(aval, bval, cval, minterm);

            if (ife)
                dval = blitter_fill_word(dval, &fill_carry, 1, desc);
            else if (efe)
                dval = blitter_fill_word(dval, &fill_carry, 0, desc);

            b->bltddat = dval;

            if (dval != 0)
                all_zero = 0;

            if (useD) {
                blitter_chip_write16(agnus, dpt, dval);
                dpt = (uint32_t)(dpt + xinc);
            }
        }

        if (useA)
            apt = desc ? (uint32_t)(apt - (uint32_t)(int32_t)b->bltamod)
                       : (uint32_t)(apt + (uint32_t)(int32_t)b->bltamod);

        if (useB)
            bpt = desc ? (uint32_t)(bpt - (uint32_t)(int32_t)b->bltbmod)
                       : (uint32_t)(bpt + (uint32_t)(int32_t)b->bltbmod);

        if (useC)
            cpt = desc ? (uint32_t)(cpt - (uint32_t)(int32_t)b->bltcmod)
                       : (uint32_t)(cpt + (uint32_t)(int32_t)b->bltcmod);

        if (useD)
            dpt = desc ? (uint32_t)(dpt - (uint32_t)(int32_t)b->bltdmod)
                       : (uint32_t)(dpt + (uint32_t)(int32_t)b->bltdmod);
    }

    blitter_set_zero(b, agnus, all_zero);

    b->bltapt = apt;
    b->bltbpt = bpt;
    b->bltcpt = cpt;
    b->bltdpt = dpt;
}

static void blitter_execute(BlitterState *b, AgnusState *agnus)
{
    if (blitter_line_mode(b)) {
        kprintf("[BLITTER] line mode fallback\n");
        blitter_execute_line_fallback(b, agnus);
        return;
    }

    blitter_execute_copy(b, agnus);
}

static void blitter_start(BlitterState *b, AgnusState *agnus)
{
    b->cycles_remaining = blitter_compute_cycles(b->bltsize);

    blitter_set_busy(b, agnus, 1);
    blitter_set_zero(b, agnus, 1);

    kprintf("[BLITTER] start bltsize=%04x cycles=%u con0=%04x con1=%04x "
            "A=%05x B=%05x C=%05x D=%05x mod=%04x/%04x/%04x/%04x\n",
            (unsigned)b->bltsize,
            (unsigned)b->cycles_remaining,
            (unsigned)b->bltcon0,
            (unsigned)b->bltcon1,
            (unsigned)(b->bltapt & 0x1FFFFFu),
            (unsigned)(b->bltbpt & 0x1FFFFFu),
            (unsigned)(b->bltcpt & 0x1FFFFFu),
            (unsigned)(b->bltdpt & 0x1FFFFFu),
            (uint16_t)b->bltamod,
            (uint16_t)b->bltbmod,
            (uint16_t)b->bltcmod,
            (uint16_t)b->bltdmod);

    agnus_intreq_clear(agnus, PAULA_INT_BLIT);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void blitter_init(BlitterState *b)
{
    memset(b, 0, sizeof(*b));

    b->bltafwm = 0xffffu;
    b->bltalwm = 0xffffu;
    b->zero = 1;
}

void blitter_reset(BlitterState *b)
{
    blitter_init(b);
}

/* ------------------------------------------------------------------------- */
/* Execution                                                                 */
/* ------------------------------------------------------------------------- */

void blitter_step(BlitterState *b, AgnusState *agnus, uint64_t ticks)
{
    if (!b->busy)
        return;

    if (ticks >= b->cycles_remaining) {
        b->cycles_remaining = 0;

        blitter_execute(b, agnus);
        blitter_set_busy(b, agnus, 0);

        kprintf("[BLITTER] complete -> PAULA_INT_BLIT\n");
        agnus_intreq_set(agnus, PAULA_INT_BLIT);
        return;
    }

    b->cycles_remaining -= (uint32_t)ticks;
}

int blitter_is_busy(const BlitterState *b)
{
    return b->busy ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* MMIO                                                                      */
/* ------------------------------------------------------------------------- */

uint16_t blitter_read_reg(const BlitterState *b, uint16_t reg)
{
    switch (reg)
    {
        case AGNUS_BLTCON0: return b->bltcon0;
        case AGNUS_BLTCON1: return b->bltcon1;
        case AGNUS_BLTAFWM: return b->bltafwm;
        case AGNUS_BLTALWM: return b->bltalwm;

        case AGNUS_BLTCPTH: return ptr_hi(b->bltcpt);
        case AGNUS_BLTCPTL: return ptr_lo(b->bltcpt);
        case AGNUS_BLTBPTH: return ptr_hi(b->bltbpt);
        case AGNUS_BLTBPTL: return ptr_lo(b->bltbpt);
        case AGNUS_BLTAPTH: return ptr_hi(b->bltapt);
        case AGNUS_BLTAPTL: return ptr_lo(b->bltapt);
        case AGNUS_BLTDPTH: return ptr_hi(b->bltdpt);
        case AGNUS_BLTDPTL: return ptr_lo(b->bltdpt);

        case AGNUS_BLTSIZE: return b->bltsize;

        case AGNUS_BLTCMOD: return (uint16_t)b->bltcmod;
        case AGNUS_BLTBMOD: return (uint16_t)b->bltbmod;
        case AGNUS_BLTAMOD: return (uint16_t)b->bltamod;
        case AGNUS_BLTDMOD: return (uint16_t)b->bltdmod;

        case AGNUS_BLTCDAT: return b->bltcdat;
        case AGNUS_BLTBDAT: return b->bltbdat;
        case AGNUS_BLTADAT: return b->bltadat;

        default:
            return 0;
    }
}

void blitter_write_reg(BlitterState *b, AgnusState *agnus, uint16_t reg, uint16_t value)
{
    switch (reg)
    {
        case AGNUS_BLTCON0:
            b->bltcon0 = value;
            return;

        case AGNUS_BLTCON1:
            b->bltcon1 = value;
            return;

        case AGNUS_BLTAFWM:
            b->bltafwm = value;
            return;

        case AGNUS_BLTALWM:
            b->bltalwm = value;
            return;

        case AGNUS_BLTCPTH:
            b->bltcpt = make_ptr(value, ptr_lo(b->bltcpt));
            return;

        case AGNUS_BLTCPTL:
            b->bltcpt = make_ptr(ptr_hi(b->bltcpt), value & 0xfffeu);
            return;

        case AGNUS_BLTBPTH:
            b->bltbpt = make_ptr(value, ptr_lo(b->bltbpt));
            return;

        case AGNUS_BLTBPTL:
            b->bltbpt = make_ptr(ptr_hi(b->bltbpt), value & 0xfffeu);
            return;

        case AGNUS_BLTAPTH:
            b->bltapt = make_ptr(value, ptr_lo(b->bltapt));
            return;

        case AGNUS_BLTAPTL:
            b->bltapt = make_ptr(ptr_hi(b->bltapt), value & 0xfffeu);
            return;

        case AGNUS_BLTDPTH:
            b->bltdpt = make_ptr(value, ptr_lo(b->bltdpt));
            return;

        case AGNUS_BLTDPTL:
            b->bltdpt = make_ptr(ptr_hi(b->bltdpt), value & 0xfffeu);
            return;

        case AGNUS_BLTCMOD:
            b->bltcmod = (int16_t)value;
            return;

        case AGNUS_BLTBMOD:
            b->bltbmod = (int16_t)value;
            return;

        case AGNUS_BLTAMOD:
            b->bltamod = (int16_t)value;
            return;

        case AGNUS_BLTDMOD:
            b->bltdmod = (int16_t)value;
            return;

        case AGNUS_BLTCDAT:
            b->bltcdat = value;
            return;

        case AGNUS_BLTBDAT:
            b->bltbdat = value;
            return;

        case AGNUS_BLTADAT:
            b->bltadat = value;
            return;

        case AGNUS_BLTSIZE:
            b->bltsize = value;
            if (!b->busy)
                blitter_start(b, agnus);
            return;

        default:
            kprintf("[BLITTER] unhandled write reg=%04x value=%04x\n",
                    (unsigned)reg,
                    (unsigned)value);
            return;
    }
}
