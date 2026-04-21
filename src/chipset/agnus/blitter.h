#pragma once

#include <stdint.h>

struct AgnusState;

/*
 * Offsets relativos à base custom 0xDFF000.
 */
enum
{
    AGNUS_BLTCON0 = 0x040,
    AGNUS_BLTCON1 = 0x042,
    AGNUS_BLTAFWM = 0x044,
    AGNUS_BLTALWM = 0x046,

    AGNUS_BLTCPTH = 0x048,
    AGNUS_BLTCPTL = 0x04A,
    AGNUS_BLTBPTH = 0x04C,
    AGNUS_BLTBPTL = 0x04E,
    AGNUS_BLTAPTH = 0x050,
    AGNUS_BLTAPTL = 0x052,
    AGNUS_BLTDPTH = 0x054,
    AGNUS_BLTDPTL = 0x056,

    AGNUS_BLTSIZE = 0x058,

    AGNUS_BLTCMOD = 0x060,
    AGNUS_BLTBMOD = 0x062,
    AGNUS_BLTAMOD = 0x064,
    AGNUS_BLTDMOD = 0x066,

    AGNUS_BLTCDAT = 0x070,
    AGNUS_BLTBDAT = 0x072,
    AGNUS_BLTADAT = 0x074
};

/*
 * Bits úteis de DMACONR/DMACON.
 */
enum
{
    AGNUS_DMACON_BBUSY = 0x4000,
    AGNUS_DMACON_BZERO = 0x2000
};

typedef struct BlitterState
{
    uint16_t bltcon0;
    uint16_t bltcon1;

    uint16_t bltafwm;
    uint16_t bltalwm;

    uint32_t bltcpt;
    uint32_t bltbpt;
    uint32_t bltapt;
    uint32_t bltdpt;

    uint16_t bltsize;

    int16_t bltcmod;
    int16_t bltbmod;
    int16_t bltamod;
    int16_t bltdmod;

    uint16_t bltcdat;
    uint16_t bltbdat;
    uint16_t bltadat;
    uint16_t bltddat;

    int busy;
    int zero;

    uint32_t cycles_remaining;
} BlitterState;

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void blitter_init(BlitterState *b);
void blitter_reset(BlitterState *b);

/* ------------------------------------------------------------------------- */
/* Execution                                                                 */
/* ------------------------------------------------------------------------- */

void blitter_step(BlitterState *b, struct AgnusState *agnus, uint64_t ticks);
int  blitter_is_busy(const BlitterState *b);

/* ------------------------------------------------------------------------- */
/* MMIO                                                                      */
/* ------------------------------------------------------------------------- */

uint16_t blitter_read_reg(const BlitterState *b, uint16_t reg);
void     blitter_write_reg(BlitterState *b, struct AgnusState *agnus, uint16_t reg, uint16_t value);