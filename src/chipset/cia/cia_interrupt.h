#ifndef BELLATRIX_CIA_INTERRUPT_H
#define BELLATRIX_CIA_INTERRUPT_H

#include <stdint.h>

typedef struct CIA_State CIA;

void    cia_interrupt_reset(CIA *cia);
void    cia_interrupt_sync_irq_line(CIA *cia);
void    cia_interrupt_raise(CIA *cia, uint8_t mask);
uint8_t cia_interrupt_read_icr(CIA *cia);
void    cia_interrupt_write_icr(CIA *cia, uint8_t value);
int     cia_irq_pending(const CIA *cia);
uint8_t cia_compute_ipl(const CIA *cia);

#endif
