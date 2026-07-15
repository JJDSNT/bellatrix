#ifndef BELLATRIX_HOST_RASPI3_PHYSICAL_INTERRUPTS_H
#define BELLATRIX_HOST_RASPI3_PHYSICAL_INTERRUPTS_H

#include <stdbool.h>
#include <stdint.h>

/* UART0 is the Pi 3 Bluetooth transport. Its normal IRQ remains routed to
 * Core 0 together with the original Emu68 physical interrupt contract. */
void bellatrix_physical_bt_irq_enable(void);
void bellatrix_physical_bt_irq_disable(void);
void bellatrix_physical_bt_irq_rearm(void);
bool bellatrix_physical_bt_irq_is_armed(void);

/* Called only by the UART0 discriminator/trampoline in Emu68 vectors.c. */
void bellatrix_physical_bt_irq_handler(void);

uint32_t bellatrix_physical_bt_irq_count(void);

#endif
