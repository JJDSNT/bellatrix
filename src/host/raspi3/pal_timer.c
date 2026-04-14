// src/host/raspi3/pal_timer.c
//
// ARM generic timer (EL1 virtual timer) configured to fire as FIQ
// at 50 Hz for VBL generation on Raspberry Pi 3.
//
// The FIQ is handled in the vectors.c patch (BELLATRIX branch of
// curr_el_spx_fiq). That handler reloads the timer, sets INTREQ[VERTB],
// and notifies the JIT loop via M68KState.INT.ARM/IPL.

#include "pal.h"
#include "chipset/agnus/agnus.h"  // for bellatrix_vbl_interval

// ---------------------------------------------------------------------------
// BCM2836 local interrupt controller (RPi3)
// Base address as mapped by Emu68: 0xF3000000
// ---------------------------------------------------------------------------
#define LOCAL_INTC_BASE         0xF3000000UL
#define LOCAL_TIMER_INT_CTRL0   (*(volatile uint32_t*)(LOCAL_INTC_BASE + 0x40))
#define LOCAL_TIMER_FIQ_CTRL0   (*(volatile uint32_t*)(LOCAL_INTC_BASE + 0x60))

// Bit positions in the FIQ/IRQ control registers:
//   bit 0 = CNTPSIRQ, bit 1 = CNTPNSIRQ, bit 2 = CNTHPIRQ, bit 3 = CNTVIRQ
#define CNTVIRQ_BIT             (1 << 3)

// ---------------------------------------------------------------------------
// Public initialisation — called from bellatrix_init()
// ---------------------------------------------------------------------------

void PAL_ChipsetTimer_Init(uint32_t hz, void (*cb)(void))
{
    (void)cb; // FIQ handler is inline assembly in vectors.c

    // Read ARM timer frequency to compute reload interval.
    uint64_t cntfrq;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(cntfrq));
    if (!cntfrq) cntfrq = 62500000ULL; // QEMU default: 62.5 MHz

    bellatrix_vbl_interval = cntfrq / hz;

    // Load the countdown timer and enable it.
    asm volatile("msr CNTV_TVAL_EL0, %0" :: "r"(bellatrix_vbl_interval));
    asm volatile("msr CNTV_CTL_EL0,  %0" :: "r"((uint64_t)1)); // enable, not masked

    // Route CNTVIRQ as FIQ on core 0.
    LOCAL_TIMER_FIQ_CTRL0 = CNTVIRQ_BIT;
}

void PAL_ChipsetTimer_Start(void)
{
    asm volatile("msr CNTV_CTL_EL0, %0" :: "r"((uint64_t)1));
}

void PAL_ChipsetTimer_Stop(void)
{
    asm volatile("msr CNTV_CTL_EL0, %0" :: "r"((uint64_t)0));
}
