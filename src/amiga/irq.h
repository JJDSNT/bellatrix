#ifndef BELLATRIX_AMIGA_IRQ_H
#define BELLATRIX_AMIGA_IRQ_H

#include "rigel/rigel.h"

void amiga_irq_init(RigelContext *rigel);
void amiga_irq_sync(void);
unsigned int amiga_irq_get_ipl(void);

#endif /* BELLATRIX_AMIGA_IRQ_H */
