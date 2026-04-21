#include "blitter.h"

#include <string.h>

#include "agnus.h"
#include "chipset/paula/paula.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

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
    return width ? width : 64;
}

static inline uint16_t blitter_height_rows(const BlitterState *b)
{
    uint16_t height = (b->bltsize >> 6) & 0x03ffu;
    return height ? height : 1024;
}

static inline uint32_t blitter_compute_cycles(uint16_t bltsize)
{
    uint16_t width  = bltsize & 0x003fu;
    uint16_t height = (bltsize >> 6) & 0x03ffu;

    if (width == 0)
        width = 64;

    if (height == 0)
        height = 1024;

    return (uint32_t)width * (uint32_t)height;
}

/*
 * BBUSY/BZERO are read-back bits in DMACONR, NOT part of the written dmacon.
 * They live in blitter struct only; agnus_read_reg(DMACONR) OR-s them in.
 */
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

/*
 * Execução pragmática:
 *
 * - aceita e mantém todos os registradores relevantes
 * - consome tempo proporcional ao BLTSIZE
 * - atualiza ponteiros/modulos de forma coerente
 * - reflete busy/zero
 * - gera INT_BLIT
 *
 * Não tenta acessar chip RAM diretamente porque o teu AgnusState atual
 * não expõe esse contrato.
 */
static void blitter_execute_pragmatic(BlitterState *b, AgnusState *agnus)
{
    const int desc = (b->bltcon1 & 0x0002u) != 0;
    const int xinc = desc ? -2 : 2;

    const uint16_t width_words = blitter_width_words(b);
    const uint16_t height_rows = blitter_height_rows(b);

    const uint32_t row_bytes = (uint32_t)width_words * 2u;

    uint32_t apt = b->bltapt;
    uint32_t bpt = b->bltbpt;
    uint32_t cpt = b->bltcpt;
    uint32_t dpt = b->bltdpt;

    /*
     * Sem acesso real à RAM, usamos um critério simples para Z:
     * se o minterm for zero e os dados latched forem zero, o resultado
     * permanece zero; caso contrário consideramos operação não-zero.
     *
     * Isso é imperfeito, mas muito melhor do que não manter nada.
     */
    {
        uint8_t minterm = (uint8_t)(b->bltcon0 & 0x00ffu);
        int maybe_zero = (minterm == 0x00) &&
                         (b->bltadat == 0) &&
                         (b->bltbdat == 0) &&
                         (b->bltcdat == 0);

        blitter_set_zero(b, agnus, maybe_zero ? 1 : 0);
    }

    for (uint16_t y = 0; y < height_rows; ++y)
    {
        apt = (uint32_t)(apt + (uint32_t)(xinc * (int)width_words));
        bpt = (uint32_t)(bpt + (uint32_t)(xinc * (int)width_words));
        cpt = (uint32_t)(cpt + (uint32_t)(xinc * (int)width_words));
        dpt = (uint32_t)(dpt + (uint32_t)(xinc * (int)width_words));

        if (desc)
        {
            apt = (uint32_t)(apt - b->bltamod);
            bpt = (uint32_t)(bpt - b->bltbmod);
            cpt = (uint32_t)(cpt - b->bltcmod);
            dpt = (uint32_t)(dpt - b->bltdmod);
        }
        else
        {
            apt = (uint32_t)(apt + b->bltamod);
            bpt = (uint32_t)(bpt + b->bltbmod);
            cpt = (uint32_t)(cpt + b->bltcmod);
            dpt = (uint32_t)(dpt + b->bltdmod);
        }
    }

    /*
     * Mantém BLTDDAT com um valor determinístico simples.
     */
    b->bltddat = (uint16_t)(
        (b->bltadat ^ b->bltbdat ^ b->bltcdat) ^
        b->bltafwm ^ b->bltalwm ^
        (uint16_t)row_bytes
    );

    b->bltapt = apt;
    b->bltbpt = bpt;
    b->bltcpt = cpt;
    b->bltdpt = dpt;
}

static void blitter_start(BlitterState *b, AgnusState *agnus)
{
    b->cycles_remaining = blitter_compute_cycles(b->bltsize);

    blitter_set_busy(b, agnus, 1);
    blitter_set_zero(b, agnus, 1);

    kprintf("[BLITTER] start bltsize=%04x cycles=%u\n",
            (unsigned)b->bltsize,
            (unsigned)b->cycles_remaining);

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

    if (ticks >= b->cycles_remaining)
    {
        b->cycles_remaining = 0;

        blitter_execute_pragmatic(b, agnus);
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