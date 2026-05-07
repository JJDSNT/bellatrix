#ifndef BELLATRIX_CHIPSET_PAULA_H
#define BELLATRIX_CHIPSET_PAULA_H

#include <stdint.h>

#include "chipset/paula/paula_input.h"
#include "chipset/paula/paula_interrupt.h"
#include "chipset/paula/paula_serial.h"
#include "chipset/paula/paula_disk.h"

struct AgnusState;
struct CIA_State;

#define PAULA_INT_MASTER PAULA_INT_INTEN

typedef struct Paula
{
    PaulaInterrupt irq;
    uint16_t irq_line_level;
    PaulaInput input;

    PaulaSerial serial;
    PaulaDisk disk;
} Paula;

/* lifecycle */
void paula_init(Paula *p);
void paula_reset(Paula *p);

/* IRQ sources — called by CIA and Agnus when events fire */
void paula_irq_raise(Paula *p, uint16_t bits);
void paula_irq_clear(Paula *p, uint16_t bits);

/* IPL derivation — called by machine to compute CPU interrupt level */
uint8_t paula_compute_ipl(const Paula *p);

/* time advance */
void paula_step(Paula *p, uint32_t ticks);

/* wiring — machine calls these during init */
void paula_attach_agnus(Paula *p, struct AgnusState *agnus);
void paula_attach_cia_a(Paula *p, struct CIA_State *cia);
void paula_attach_cia_b(Paula *p, struct CIA_State *cia);
void paula_attach_memory(Paula *p, uint8_t *chipram, size_t size);
void paula_attach_drive(Paula *p, FloppyDrive *drive);
void paula_set_mouse_right(Paula *p, unsigned port, int pressed);

/* bus protocol — called by machine.c read/write dispatch */
int paula_handles_read(const Paula *p, uint32_t addr);
int paula_handles_write(const Paula *p, uint32_t addr);
uint32_t paula_read(Paula *p, uint32_t addr, unsigned int size);
void paula_write(Paula *p, uint32_t addr, uint32_t value, unsigned int size);

#endif
