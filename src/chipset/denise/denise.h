// src/chipset/denise/denise.h

#ifndef _BELLATRIX_DENISE_H
#define _BELLATRIX_DENISE_H

#include <stdint.h>

// Forward declare to avoid circular include (agnus.h ← denise.h ← agnus.h)
typedef struct AgnusState AgnusState;

// ---------------------------------------------------------------------------
// Register offsets (decoded, same convention as agnus.h)
// ---------------------------------------------------------------------------

// Display window / data fetch — written by CPU/copper, used by Agnus+Denise.
// Offset definitions live in agnus.h (AGNUS_DIWSTRT etc); Denise reads them
// from AgnusState at render time.

// Bitplane control (Denise-owned)
#define DENISE_BPLCON0  0x0100u
#define DENISE_BPLCON1  0x0102u
#define DENISE_BPLCON2  0x0104u
#define DENISE_BPL1MOD  0x0108u
#define DENISE_BPL2MOD  0x010Au

// Colour registers: 32 entries at 2-byte intervals starting at offset 0x180
#define DENISE_COLOR_BASE 0x0180u
#define DENISE_COLOR_END  0x01BEu

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void     denise_init(void);

// Write a Denise register.  reg is the decoded offset (addr & 0x1FE),
// matching the convention used by agnus_write_reg.
void     denise_write(uint16_t reg, uint16_t value);

// Render one Amiga frame into the Emu68 framebuffer.
// BPLxPT, DIW, and DDF are read from agnus (Agnus-owned DMA config).
// BPLCON, MOD, and COLOR are read from Denise's own state.
// Called from agnus_step() after copper_vbl_execute().
void     denise_render_frame(const AgnusState *agnus);

#endif /* _BELLATRIX_DENISE_H */
