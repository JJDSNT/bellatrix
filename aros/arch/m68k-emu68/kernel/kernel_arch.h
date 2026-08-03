/*
 * Machine-specific definitions for arch/m68k-emu68.
 *
 * Real BCM283x IRQ enable/disable, implemented in platform.c against
 * whichever interrupt controller driver platform_timer_start() discovered
 * (see arch/m68k-emu68/platform/). This overrides the generic no-op
 * default in rom/kernel/kernel_arch.h.
 */
#include <inttypes.h>

#define IRQ_COUNT 72

extern void ictl_enable_irq(uint8_t irq, struct KernelBase *KernelBase);
extern void ictl_disable_irq(uint8_t irq, struct KernelBase *KernelBase);
