// src/chipset/denise/denise.h
//
// Denise — bitplane compositor and colour palette.
// Phase 4: up to 6 bitplanes, 32-colour palette, PAL low-res / hi-res.

#ifndef _BELLATRIX_DENISE_H
#define _BELLATRIX_DENISE_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Register addresses (Denise-side and shared Agnus/Denise)
// ---------------------------------------------------------------------------

// Display window / data fetch (written by CPU/copper, used by Agnus+Denise)
#define DENISE_DIWSTRT  0xDFF08E
#define DENISE_DIWSTOP  0xDFF090
#define DENISE_DDFSTRT  0xDFF092
#define DENISE_DDFSTOP  0xDFF094

// Bitplane DMA pointers (Agnus-side, written by CPU/copper)
#define AGNUS_BPL1PTH   0xDFF0E0
#define AGNUS_BPL1PTL   0xDFF0E2
#define AGNUS_BPL2PTH   0xDFF0E4
#define AGNUS_BPL2PTL   0xDFF0E6
#define AGNUS_BPL3PTH   0xDFF0E8
#define AGNUS_BPL3PTL   0xDFF0EA
#define AGNUS_BPL4PTH   0xDFF0EC
#define AGNUS_BPL4PTL   0xDFF0EE
#define AGNUS_BPL5PTH   0xDFF0F0
#define AGNUS_BPL5PTL   0xDFF0F2
#define AGNUS_BPL6PTH   0xDFF0F4
#define AGNUS_BPL6PTL   0xDFF0F6

// Bitplane control (Denise)
#define DENISE_BPLCON0  0xDFF100
#define DENISE_BPLCON1  0xDFF102
#define DENISE_BPLCON2  0xDFF104
#define DENISE_BPL1MOD  0xDFF108
#define DENISE_BPL2MOD  0xDFF10A

// Colour registers (Denise) — 32 entries at 2-byte intervals
#define DENISE_COLOR00  0xDFF180
#define DENISE_COLOR31  0xDFF1BE

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void     denise_init(void);
void     denise_write(uint32_t addr, uint32_t value);
uint32_t denise_read(uint32_t addr);

// Render one Amiga frame into the VC4 framebuffer.
// Called from agnus_vbl_fire() before raising INT_VERTB.
void     denise_render_frame(void);

#endif /* _BELLATRIX_DENISE_H */
