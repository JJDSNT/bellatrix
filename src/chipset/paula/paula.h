#ifndef BELLATRIX_CHIPSET_PAULA_H
#define BELLATRIX_CHIPSET_PAULA_H

#include <stdint.h>

struct AgnusState;
struct CIA_State;

/* INTREQ / INTENA bit definitions */
enum {
    PAULA_INT_TBE    = 1u << 0,
    PAULA_INT_DSKBLK = 1u << 1,
    PAULA_INT_SOFT   = 1u << 2,
    PAULA_INT_PORTS  = 1u << 3,
    PAULA_INT_COPER  = 1u << 4,
    PAULA_INT_VERTB  = 1u << 5,
    PAULA_INT_BLIT   = 1u << 6,
    PAULA_INT_AUD0   = 1u << 7,
    PAULA_INT_AUD1   = 1u << 8,
    PAULA_INT_AUD2   = 1u << 9,
    PAULA_INT_AUD3   = 1u << 10,
    PAULA_INT_RBF    = 1u << 11,
    PAULA_INT_DSKSYN = 1u << 12,
    PAULA_INT_EXTER  = 1u << 13,
    PAULA_INT_MASTER = 1u << 14,   /* INTENA master enable (SET/CLR bit 15 on write) */
};

typedef struct Paula {
    uint16_t intreq;
    uint16_t intena;
    uint8_t  ipl;
    uint32_t disk_dma_countdown;   /* CPU cycles until DSKBLK fires (0 = inactive) */
} Paula;

/* lifecycle */
void paula_init(Paula *p);
void paula_reset(Paula *p);

/* IRQ sources — called by CIA and Agnus when events fire */
void paula_irq_raise(Paula *p, uint16_t bits);
void paula_irq_clear(Paula *p, uint16_t bits);

/* IPL derivation — called by machine to compute CPU interrupt level */
uint8_t paula_compute_ipl(const Paula *p);

/* time advance — no-op for now; reserved for audio DMA */
void paula_step(Paula *p, uint32_t ticks);

/* wiring — machine calls these during init */
void paula_attach_agnus(Paula *p, struct AgnusState *agnus);
void paula_attach_cia_a(Paula *p, struct CIA_State *cia);
void paula_attach_cia_b(Paula *p, struct CIA_State *cia);

/* bus protocol — called by machine.c read/write dispatch */
int      paula_handles_read(const Paula *p, uint32_t addr);
int      paula_handles_write(const Paula *p, uint32_t addr);
uint32_t paula_read(Paula *p, uint32_t addr, unsigned int size);
void     paula_write(Paula *p, uint32_t addr, uint32_t value, unsigned int size);

#endif
