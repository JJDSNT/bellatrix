// src/host/raspi3/pal_core.c
//
// Platform timer, core management and video stubs for RPi3.
// PAL_ChipsetTimer_Init configures the ARM EL1 virtual timer as FIQ at
// the given frequency (50 Hz for PAL VBL). The FIQ fires to the Bellatrix
// handler in vectors.c (curr_el_spx_fiq / #ifdef BELLATRIX).

#include "pal.h"
#include "chipset/agnus/agnus.h"  // bellatrix_vbl_interval

// ---------------------------------------------------------------------------
// BCM2836 local interrupt controller (RPi3, mapped at 0xF3000000 by Emu68)
// ---------------------------------------------------------------------------
#define LOCAL_INTC_BASE       0xF3000000UL
#define LOCAL_TIMER_FIQ_CTRL0 (*(volatile uint32_t*)(LOCAL_INTC_BASE + 0x60))
#define CNTVIRQ_FIQ_BIT       (1u << 3)  // CNTVIRQ as FIQ on core 0

void PAL_ChipsetTimer_Init(uint32_t hz, void (*cb)(void))
{
    (void)cb; // FIQ handler is inline assembly in vectors.c

    uint64_t cntfrq;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(cntfrq));
    if (!cntfrq) cntfrq = 62500000ULL; // QEMU raspi3b default

    bellatrix_vbl_interval = cntfrq / hz;

    asm volatile("msr CNTV_TVAL_EL0, %0" :: "r"(bellatrix_vbl_interval));
    asm volatile("msr CNTV_CTL_EL0,  %0" :: "r"((uint64_t)1)); // enable

    LOCAL_TIMER_FIQ_CTRL0 = CNTVIRQ_FIQ_BIT;
}

void PAL_ChipsetTimer_Start(void)
{
    asm volatile("msr CNTV_CTL_EL0, %0" :: "r"((uint64_t)1));
}

void PAL_ChipsetTimer_Stop(void)
{
    asm volatile("msr CNTV_CTL_EL0, %0" :: "r"((uint64_t)0));
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
