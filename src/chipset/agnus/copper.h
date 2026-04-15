// src/chipset/agnus/copper.h
//
// Copper co-processor — register interface and API.
// Phase 4: batch execution at VBL time (all MOVEs applied before render).

#ifndef _BELLATRIX_COPPER_H
#define _BELLATRIX_COPPER_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Register addresses
// ---------------------------------------------------------------------------

#define COPPER_COP1LCH  0xDFF080  // Copper list 1 pointer, high word [20:16]
#define COPPER_COP1LCL  0xDFF082  // Copper list 1 pointer, low word  [15:1]
#define COPPER_COP2LCH  0xDFF084  // Copper list 2 pointer, high word [20:16]
#define COPPER_COP2LCL  0xDFF086  // Copper list 2 pointer, low word  [15:1]
#define COPPER_COPJMP1  0xDFF088  // Strobe: restart copper from COP1LC
#define COPPER_COPJMP2  0xDFF08A  // Strobe: restart copper from COP2LC
#define COPPER_COPINS   0xDFF08C  // Copper instruction write (CPU direct, rare)

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void     copper_init(void);
void     copper_write(uint32_t addr, uint16_t value);
uint32_t copper_read(uint32_t addr);

// Execute the copper list. Called from agnus_vbl_fire() before rendering.
// Reloads the PC from COP1LC each VBL and dispatches all MOVE instructions
// to the chipset register write path (agnus_write).
void copper_vbl_execute(void);

#endif /* _BELLATRIX_COPPER_H */
