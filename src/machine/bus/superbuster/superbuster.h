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

void    superbuster_init(SuperBusterState *s);
void    superbuster_reset(SuperBusterState *s);
int     superbuster_owns(uint32_t addr);
uint8_t superbuster_read8(SuperBusterState *s, uint32_t addr);
void    superbuster_write8(SuperBusterState *s, uint32_t addr, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif
