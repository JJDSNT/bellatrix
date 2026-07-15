#ifndef BELLATRIX_HOST_RASPI3_PHYSICAL_INTERRUPTS_H
#define BELLATRIX_HOST_RASPI3_PHYSICAL_INTERRUPTS_H

#include <stdbool.h>
#include <stdint.h>

/* UART0 is the Pi 3 Bluetooth transport. Its normal IRQ is handled entirely
 * on the ARM side and is never implicitly propagated to AmigaOS. */
void bellatrix_physical_bt_irq_enable(void);
void bellatrix_physical_bt_irq_disable(void);
void bellatrix_physical_bt_irq_rearm(void);
bool bellatrix_physical_bt_irq_is_armed(void);

enum {
    BELLATRIX_PHYSICAL_IRQ_UNKNOWN = 0u,
    BELLATRIX_PHYSICAL_IRQ_UART0 = 1u,
};

/* Called only by the physical-IRQ discriminator/trampoline in vectors.c. */
void bellatrix_physical_irq_handler(uint32_t source);

uint32_t bellatrix_physical_bt_irq_count(void);
uint32_t bellatrix_physical_unknown_irq_count(void);

#endif
