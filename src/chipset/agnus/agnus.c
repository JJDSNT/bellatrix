// src/chipset/agnus/agnus.c
//
// Agnus/Alice — INTENA, INTREQ, DMACON, beam position simulation.
// Phase 3: interrupt routing only.

#include "agnus.h"
#include "host/pal.h"
#include "chipset/cia/cia.h"
#include "chipset/denise/denise.h"
#include "core/btrace.h"

// ---------------------------------------------------------------------------
// Global state — accessible by name from FIQ handler assembly (via adrp).
// ---------------------------------------------------------------------------
uint16_t bellatrix_intena    = 0;
uint16_t bellatrix_intreq    = 0;
uint16_t bellatrix_dmacon    = 0;
uint64_t bellatrix_vbl_interval = 0; // set by pal_timer_init

// Frame start tick for beam position simulation.
static uint64_t s_frame_start = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint64_t read_cntpct(void)
{
    uint64_t v;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(v));
    return v;
}

// ---------------------------------------------------------------------------
// IPL priority encoder
// ---------------------------------------------------------------------------

uint8_t agnus_compute_ipl(void)
{
    if (!(bellatrix_intena & INT_INTEN)) return 0;
    uint16_t pending = bellatrix_intena & bellatrix_intreq & 0x3FFF;
    if (!pending) return 0;
    if (pending & INT_EXTER)                    return 6;
    if (pending & (INT_RBF   | INT_DSKBYT))    return 5;
    if (pending & (INT_AUD0  | INT_AUD1 |
                   INT_AUD2  | INT_AUD3))       return 4;
    if (pending & (INT_VERTB | INT_BLIT | INT_COPER)) return 3;
    if (pending & INT_PORTS)                    return 2;
    if (pending & (INT_TBE   | INT_DSKBLK | INT_SOFTINT)) return 1;
    return 0;
}

// Notify the JIT loop of the current IPL.
static void notify_ipl(void)
{
    uint8_t ipl = agnus_compute_ipl();
    if (ipl) PAL_IPL_Set(ipl);
    else     PAL_IPL_Clear();
}

// ---------------------------------------------------------------------------
// Public: set / clear INTREQ bits
// ---------------------------------------------------------------------------

void agnus_intreq_set(uint16_t bits)
{
    bellatrix_intreq |= bits;
    notify_ipl();
}

void agnus_intreq_clear(uint16_t bits)
{
    bellatrix_intreq &= ~bits;
    notify_ipl();
}

// ---------------------------------------------------------------------------
// VBL — called from FIQ handler (via pal_timer.c) or beam-position polling.
// ---------------------------------------------------------------------------

// Forward declaration from bellatrix.c / chipset integration.
extern void bellatrix_cia_vbl_tick(void);

void agnus_vbl_fire(void)
{
    s_frame_start = read_cntpct();
    denise_render_frame();
    agnus_intreq_set(INT_VERTB);
    bellatrix_cia_vbl_tick();
    btrace_watchdog_tick();
}

// ---------------------------------------------------------------------------
// Beam position (simulated from real time)
// ---------------------------------------------------------------------------

// PAL: 313 lines × 227 colour clocks = 71051 ticks per frame
// We simulate using ARM CNTPCT relative to frame start.
static void get_beam(uint16_t *vposr_out, uint16_t *vhposr_out)
{
    if (!bellatrix_vbl_interval) {
        *vposr_out = 0; *vhposr_out = 0x0100; // line 1
        return;
    }
    uint64_t elapsed = read_cntpct() - s_frame_start;
    if (elapsed > bellatrix_vbl_interval)
        elapsed = bellatrix_vbl_interval - 1;

    // Scale to 313 lines
    uint32_t line = (uint32_t)((elapsed * 313) / bellatrix_vbl_interval);
    // Scale to 227 colour clocks within line
    uint64_t line_ticks = bellatrix_vbl_interval / 313;
    uint32_t hpos = line_ticks ? (uint32_t)((elapsed % line_ticks) * 227 / line_ticks) : 0;

    // VPOSR: bit 0 = bit 8 of vertical counter (set when line >= 256)
    *vposr_out  = (line >= 256) ? 0x0001 : 0x0000;
    // VHPOSR: [15:8] = line [7:0], [7:0] = horizontal position
    *vhposr_out = (uint16_t)(((line & 0xFF) << 8) | (hpos & 0xFF));
}

// ---------------------------------------------------------------------------
// Register dispatch
// ---------------------------------------------------------------------------

void agnus_init(void)
{
    bellatrix_intena = 0;
    bellatrix_intreq = 0;
    bellatrix_dmacon = 0;
    s_frame_start    = read_cntpct();
    denise_init();
}

uint32_t agnus_read(uint32_t addr)
{
    switch (addr) {
    case AGNUS_DMACONR:
        return bellatrix_dmacon;

    case AGNUS_VPOSR: {
        uint16_t vposr, vhposr;
        get_beam(&vposr, &vhposr);
        return vposr;
    }
    case AGNUS_VHPOSR: {
        uint16_t vposr, vhposr;
        get_beam(&vposr, &vhposr);
        return vhposr;
    }

    case AGNUS_INTENAR:
        return bellatrix_intena;

    case AGNUS_INTREQR:
        return bellatrix_intreq;

    default:
        return denise_read(addr);
    }
}

void agnus_write(uint32_t addr, uint32_t value, int size)
{
    (void)size;
    uint16_t v = (uint16_t)value;

    switch (addr) {
    case AGNUS_INTENA:
        if (v & 0x8000) bellatrix_intena |=  (v & 0x7FFF);
        else            bellatrix_intena &= ~(v & 0x7FFF);
        notify_ipl();
        break;

    case AGNUS_INTREQ:
        // Bit 15=1 → set bits; bit 15=0 → clear bits (acknowledge).
        if (v & 0x8000) bellatrix_intreq |=  (v & 0x3FFF);
        else            bellatrix_intreq &= ~(v & 0x3FFF);
        notify_ipl();
        break;

    case AGNUS_DMACON:
        if (v & 0x8000) bellatrix_dmacon |=  (v & 0x7FFF);
        else            bellatrix_dmacon &= ~(v & 0x7FFF);
        break;

    default:
        denise_write(addr, v);
        break;
    }
}
