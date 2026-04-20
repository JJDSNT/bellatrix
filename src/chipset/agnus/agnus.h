#ifndef BELLATRIX_CHIPSET_AGNUS_H
#define BELLATRIX_CHIPSET_AGNUS_H

#include <stdint.h>

#include "blitter.h"
#include "copper.h"

struct Denise;
struct Paula;

/* ---------------------------------------------------------------------------
 * Register offsets (already decoded by bus/router)
 * ------------------------------------------------------------------------- */

#define AGNUS_DMACONR 0x0002u
#define AGNUS_VPOSR   0x0004u
#define AGNUS_VHPOSR  0x0006u

#define AGNUS_DIWSTRT 0x008Eu
#define AGNUS_DIWSTOP 0x0090u
#define AGNUS_DDFSTRT 0x0092u
#define AGNUS_DDFSTOP 0x0094u

#define AGNUS_DMACON  0x0096u

/* Bitplane pointers */
#define AGNUS_BPL1PTH 0x00E0u
#define AGNUS_BPL1PTL 0x00E2u
#define AGNUS_BPL2PTH 0x00E4u
#define AGNUS_BPL2PTL 0x00E6u
#define AGNUS_BPL3PTH 0x00E8u
#define AGNUS_BPL3PTL 0x00EAu
#define AGNUS_BPL4PTH 0x00ECu
#define AGNUS_BPL4PTL 0x00EEu
#define AGNUS_BPL5PTH 0x00F0u
#define AGNUS_BPL5PTL 0x00F2u
#define AGNUS_BPL6PTH 0x00F4u
#define AGNUS_BPL6PTL 0x00F6u

/* Blitter */
#define AGNUS_BLTCON0 0x0040u
#define AGNUS_BLTCON1 0x0042u
#define AGNUS_BLTCPTH 0x0048u
#define AGNUS_BLTCPTL 0x004Au
#define AGNUS_BLTBPTH 0x004Cu
#define AGNUS_BLTBPTL 0x004Eu
#define AGNUS_BLTAPTH 0x0050u
#define AGNUS_BLTAPTL 0x0052u
#define AGNUS_BLTDPTH 0x0054u
#define AGNUS_BLTDPTL 0x0056u
#define AGNUS_BLTSIZE 0x0058u
#define AGNUS_BLTCMOD 0x0060u
#define AGNUS_BLTBMOD 0x0062u
#define AGNUS_BLTAMOD 0x0064u
#define AGNUS_BLTDMOD 0x0066u

/* Copper */
#define AGNUS_COP1LCH 0x0080u
#define AGNUS_COP1LCL 0x0082u
#define AGNUS_COP2LCH 0x0084u
#define AGNUS_COP2LCL 0x0086u
#define AGNUS_COPJMP1 0x0088u
#define AGNUS_COPJMP2 0x008Au
#define AGNUS_COPINS  0x008Cu

/* ---------------------------------------------------------------------------
 * DMACON bits
 * ------------------------------------------------------------------------- */

#ifndef DMAF_BLTEN
#define DMAF_BLTEN (1u << 6)
#endif

#ifndef DMAF_DMAEN
#define DMAF_DMAEN (1u << 9)
#endif

/* ---------------------------------------------------------------------------
 * Timing — units are M68K CPU cycles (7.09 MHz PAL)
 * PAL: 313 lines × 454 cycles/line
 * ------------------------------------------------------------------------- */

#define AGNUS_PAL_LINES 313u
#define AGNUS_PAL_HPOS  454u

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

typedef struct AgnusState {
    uint16_t dmacon;

    uint32_t hpos;
    uint32_t vpos;
    uint32_t vbl_count;

    /* Display window / data fetch */
    uint16_t diwstrt;
    uint16_t diwstop;
    uint16_t ddfstrt;
    uint16_t ddfstop;

    /* Bitplane pointers (6 planes, high/low word) */
    uint16_t bplpth[6];
    uint16_t bplptl[6];

    BlitterState blitter;
    CopperState  copper;

    /* Wiring */
    struct Denise *denise;
    struct Paula  *paula;
} AgnusState;

typedef AgnusState Agnus;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void agnus_init(Agnus *s);
void agnus_reset(Agnus *s);

/* ---------------------------------------------------------------------------
 * Wiring
 * ------------------------------------------------------------------------- */

void agnus_attach_denise(Agnus *s, struct Denise *d);
void agnus_attach_paula(Agnus *s, struct Paula *p);

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

void agnus_step(Agnus *s, uint64_t ticks);

/* ---------------------------------------------------------------------------
 * IRQ forwarding (forwards to attached Paula)
 * ------------------------------------------------------------------------- */

void agnus_intreq_set(Agnus *s, uint16_t bits);
void agnus_intreq_clear(Agnus *s, uint16_t bits);

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

int agnus_blitter_busy(const Agnus *s);

/* ---------------------------------------------------------------------------
 * Bus protocol (full address, called by machine)
 * ------------------------------------------------------------------------- */

int      agnus_handles_read(const Agnus *s, uint32_t addr);
int      agnus_handles_write(const Agnus *s, uint32_t addr);
uint32_t agnus_read(Agnus *s, uint32_t addr, unsigned int size);
void     agnus_write(Agnus *s, uint32_t addr, uint32_t value, unsigned int size);

/* ---------------------------------------------------------------------------
 * Low-level register API (used by Copper and bus handler)
 * ------------------------------------------------------------------------- */

uint32_t agnus_read_reg(Agnus *s, uint16_t reg);
void     agnus_write_reg(Agnus *s, uint16_t reg, uint32_t value, int size);

#endif
