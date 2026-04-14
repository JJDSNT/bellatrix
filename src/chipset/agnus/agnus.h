// src/chipset/agnus/agnus.h
//
// Agnus/Alice — INTENA, INTREQ, DMACON, beam position (VPOSR/VHPOSR).
// Phase 3: interrupt routing only. DMA, copper, bitplanes deferred.

#ifndef _BELLATRIX_AGNUS_H
#define _BELLATRIX_AGNUS_H

#include <stdint.h>

// ---- Register addresses ----
#define AGNUS_DMACONR  0xDFF002
#define AGNUS_VPOSR    0xDFF004
#define AGNUS_VHPOSR   0xDFF006
#define AGNUS_INTENAR  0xDFF01C
#define AGNUS_INTREQR  0xDFF01E
#define AGNUS_DMACON   0xDFF096
#define AGNUS_INTENA   0xDFF09A
#define AGNUS_INTREQ   0xDFF09C

// ---- INTREQ / INTENA bit positions ----
#define INT_TBE      (1 << 0)   // Serial transmit buffer empty  → IPL 1
#define INT_DSKBLK   (1 << 1)   // Disk block done               → IPL 1
#define INT_SOFTINT  (1 << 2)   // Software interrupt            → IPL 1
#define INT_PORTS    (1 << 3)   // CIA-A (keyboard, timers)      → IPL 2
#define INT_COPER    (1 << 4)   // Copper                        → IPL 3
#define INT_VERTB    (1 << 5)   // Vertical blank                → IPL 3
#define INT_BLIT     (1 << 6)   // Blitter done                  → IPL 3
#define INT_AUD0     (1 << 7)   // Audio channel 0               → IPL 4
#define INT_AUD1     (1 << 8)   // Audio channel 1               → IPL 4
#define INT_AUD2     (1 << 9)   // Audio channel 2               → IPL 4
#define INT_AUD3     (1 << 10)  // Audio channel 3               → IPL 4
#define INT_RBF      (1 << 11)  // Serial receive buffer full    → IPL 5
#define INT_DSKBYT   (1 << 12)  // Disk byte ready               → IPL 5
#define INT_EXTER    (1 << 13)  // CIA-B (drives, timers)        → IPL 6
#define INT_INTEN    (1 << 14)  // Master interrupt enable

// ---- Global state — also accessed by FIQ handler assembly (adrp). ----
// Must be non-static globals. Defined in agnus.c.
extern uint16_t bellatrix_intena;
extern uint16_t bellatrix_intreq;
extern uint16_t bellatrix_dmacon;
extern uint64_t bellatrix_vbl_interval; // ARM ticks per 50 Hz frame; set by pal_timer_init

// ---- API ----
void     agnus_init(void);
uint32_t agnus_read(uint32_t addr);
void     agnus_write(uint32_t addr, uint32_t value, int size);

// Set a bit in INTREQ (called by CIA/floppy/etc. to raise interrupt).
void agnus_intreq_set(uint16_t bits);
// Clear a bit in INTREQ (called when Kickstart acknowledges).
void agnus_intreq_clear(uint16_t bits);

// Compute the current M68K IPL from INTENA & INTREQ.
uint8_t agnus_compute_ipl(void);

// Called from VBL path: set INTREQ[VERTB] and tick CIA TOD.
// Declared here so pal_timer.c can call it without pulling in all of chipset.
void agnus_vbl_fire(void);

#endif /* _BELLATRIX_AGNUS_H */
