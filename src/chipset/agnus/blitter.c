// src/chipset/agnus/blitter.c

#include "blitter.h"
#include "agnus.h"
#include "support.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t blitter_compute_cycles(uint16_t bltsize)
{
    uint16_t width  = bltsize & 0x003Fu;
    uint16_t height = (bltsize >> 6) & 0x03FFu;

    if (width == 0) {
        width = 64;
    }

    if (height == 0) {
        height = 1024;
    }

    return (uint32_t)width * (uint32_t)height;
}

static void blitter_start(BlitterState *b, AgnusState *agnus)
{
    b->busy = 1;
    b->cycles_remaining = blitter_compute_cycles(b->bltsize);

    kprintf("[BLITTER] start bltsize=%04x cycles=%u\n",
            (unsigned)b->bltsize,
            (unsigned)b->cycles_remaining);

    // while busy, clear any previous completion interrupt latch
    agnus_intreq_clear(agnus, INT_BLIT);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void blitter_init(BlitterState *b)
{
    b->bltcon0 = 0;
    b->bltcon1 = 0;
    b->bltsize = 0;

    b->busy = 0;
    b->cycles_remaining = 0;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void blitter_step(BlitterState *b, AgnusState *agnus, uint64_t ticks)
{
    if (!b->busy) {
        return;
    }

    if (ticks >= b->cycles_remaining) {
        b->busy = 0;
        b->cycles_remaining = 0;

        kprintf("[BLITTER] complete -> INT_BLIT\n");

        agnus_intreq_set(agnus, INT_BLIT);
        return;
    }

    b->cycles_remaining -= (uint32_t)ticks;
}

int blitter_is_busy(const BlitterState *b)
{
    return b->busy ? 1 : 0;
}

// ---------------------------------------------------------------------------
// MMIO
// ---------------------------------------------------------------------------

void blitter_write_reg(BlitterState *b, AgnusState *agnus, uint16_t reg, uint16_t value)
{
    switch (reg) {
    case AGNUS_BLTCON0:
        b->bltcon0 = value;
        return;

    case AGNUS_BLTCON1:
        b->bltcon1 = value;
        return;

    case AGNUS_BLTSIZE:
        b->bltsize = value;
        blitter_start(b, agnus);
        return;

    default:
        kprintf("[BLITTER] unhandled write reg=%04x value=%04x\n",
                (unsigned)reg,
                (unsigned)value);
        return;
    }
}