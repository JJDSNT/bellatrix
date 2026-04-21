// src/host/raspi3/pal_ipl.c
//
// IPL injection into the Emu68 JIT loop.
//
// Uses __m68k_state — the global pointer set by M68K_StartEmu() in start.c
// before MainLoop() runs.  This is the same pointer used by ps32_protocol.c
// in the PiStorm housekeeper (__m68k_state->INT.IPL = ...).
//
// Safe to call from Core 1 because __m68k_state always points to Core 0's
// M68KState regardless of which core calls PAL_IPL_Set.
//
// PAL_IPL_Set also issues SEV so that Core 0 exits WFE (STOP instruction)
// immediately when an interrupt becomes pending.

#include "pal.h"
#include "M68k.h"   // struct M68KState, INT fields

// Defined in emu68/src/aarch64/start.c; set before MainLoop() runs.
extern struct M68KState *__m68k_state;

void PAL_IPL_Set(uint8_t ipl_level)
{
    struct M68KState *ctx = __m68k_state;
    if (!ctx) return;
    ctx->INT.IPL = ipl_level;
    // ARM field is only for PiStorm GPIO async signal.
    // Bellatrix reads INT.IPL directly; INT32 != 0 when IPL != 0, so the JIT
    // loop wakes up without needing ARM to be set.
    ctx->INT.ARM = 0;
    asm volatile("dmb ish" ::: "memory");
    // SEV wakes Core 0 from WFE (STOP) so it sees the new IPL immediately.
    asm volatile("dsb sy\n\t" "sev\n" ::: "memory");
}

void PAL_IPL_Clear(void)
{
    struct M68KState *ctx = __m68k_state;
    if (!ctx) return;
    ctx->INT.ARM = 0;
    ctx->INT.IPL = 0;
    asm volatile("dmb ish" ::: "memory");
}
