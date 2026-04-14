// src/variants/bellatrix/platform/raspi3/pal_core.c
//
// Dedicated ARM core management — Phase 0 stub.
// PAL_Core_LaunchChipset is implemented in Phase 3 when the chipset
// loop moves to a dedicated ARM core via the RPi spin-table.

#include "pal.h"

void PAL_ChipsetTimer_Init(uint32_t hz, void (*cb)(void))
{
    (void)hz;
    (void)cb;
    // Phase 3
}

void PAL_ChipsetTimer_Start(void)
{
    // Phase 3
}

void PAL_ChipsetTimer_Stop(void)
{
    // Phase 3
}

int PAL_Video_Init(uint32_t w, uint32_t h, uint32_t bpp)
{
    (void)w; (void)h; (void)bpp;
    return 0; // Phase 4
}

uint32_t *PAL_Video_GetBuffer(void)
{
    return 0; // Phase 4
}

void PAL_Video_Flip(void)
{
    // Phase 4
}

void PAL_Video_SetPalette(uint8_t idx, uint32_t rgb)
{
    (void)idx; (void)rgb; // Phase 4
}

void PAL_Core_LaunchChipset(void (*entry)(void))
{
    (void)entry; // Phase 3
}

void PAL_Core_Sync(void)
{
    asm volatile("dmb ish" ::: "memory");
}
