// src/host/raspi3/pal_ipl.c
//
// IPL injection into the Emu68 JIT loop.
// The chipset core calls PAL_IPL_Set() after updating INTREQ/INTENA;
// the JIT loop picks it up at the next interrupt-check boundary.

#include "pal.h"
#include "M68k.h"   // struct M68KState, INT fields

// TPIDRRO_EL0 holds the pointer to the current M68KState context.
static inline struct M68KState *get_m68k_ctx(void)
{
    struct M68KState *ctx;
    asm volatile("mrs %0, TPIDRRO_EL0" : "=r"(ctx));
    return ctx;
}

void PAL_IPL_Set(uint8_t ipl_level)
{
    struct M68KState *ctx = get_m68k_ctx();
    if (!ctx) return;
    ctx->INT.IPL = ipl_level;
    // DMB ensures the JIT sees IPL before ARM. INT.ARM carries the IPL
    // value so the execution-loop label-9 comparison at 998 works correctly:
    //   cmp w1(IPL), w10(ARM) — both hold the same value, highest wins.
    asm volatile("dmb ish" ::: "memory");
    ctx->INT.ARM = ipl_level;
}

void PAL_IPL_Clear(void)
{
    struct M68KState *ctx = get_m68k_ctx();
    ctx->INT.ARM = 0;
    ctx->INT.IPL = 0;
    asm volatile("dmb ish" ::: "memory");
}
