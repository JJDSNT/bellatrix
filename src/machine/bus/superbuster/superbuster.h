#ifndef BELLATRIX_BUS_SUPERBUSTER_H
#define BELLATRIX_BUS_SUPERBUSTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Super Buster chip — present in A3000/A4000 chipsets.
 * Mapped at $DD0000–$DD0FFF.
 *
 * The key register for Zorro 3 is the control register at $DD0000:
 *   bit 1 (NBSTAB): indicates Zorro 3 bus is available
 */

#define SUPERBUSTER_BASE  0x00DD0000u
#define SUPERBUSTER_END   0x00DD0FFFu

#define SUPERBUSTER_NBSTAB  0x02u  /* bit 1: non-buffered storage available */
#define SUPERBUSTER_BUSTER_ID 0xF0u  /* upper nibble = chip revision */

typedef struct SuperBusterState {
    uint8_t ctrl;   /* control register ($DD0000) */
} SuperBusterState;

/*
 * Zorro III space decode. On real hardware the Super Buster is the gate array
 * that decodes and arbitrates the Z3 bus; here it is the single authority that
 * classifies a 32-bit CPU-space address (above the 24-bit Amiga bus) against
 * the configured Z3 slots. DIRECT board backing (RAM/ROM) is installed in the
 * backend (Emu68 MMU / Musashi bank) and never reaches this decode; only
 * EXTERNAL board register windows and unmapped space do.
 */
typedef enum SuperBusterZ3Decode {
    SUPERBUSTER_Z3_UNMAPPED = 0, /* open bus */
    SUPERBUSTER_Z3_BOARD         /* configured Z3 board window (EXTERNAL) */
} SuperBusterZ3Decode;

void    superbuster_init(SuperBusterState *s);
void    superbuster_reset(SuperBusterState *s);
int     superbuster_owns(uint32_t addr);
uint8_t superbuster_read8(SuperBusterState *s, uint32_t addr);
void    superbuster_write8(SuperBusterState *s, uint32_t addr, uint8_t value);

/* Decode a 32-bit CPU-space address against the Z3 slots. Returns UNMAPPED
 * unless the Z3 bus is enabled (NBSTAB) and a configured board window contains
 * the address. */
SuperBusterZ3Decode superbuster_decode_z3(const SuperBusterState *s,
                                          uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif
