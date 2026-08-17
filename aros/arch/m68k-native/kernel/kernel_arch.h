/*
 * Kernel definitions for the m68k port.
 *
 * Real IRQ enable/disable, implemented in platform.c against whichever
 * interrupt controller driver platform_timer_start() discovered (see
 * ../platform/). This overrides the generic no-op default in
 * rom/kernel/kernel_arch.h.
 *
 * IRQ_COUNT reads like the SoC parameter that sizes the kernel's handler table.
 * It is not. Nothing in the tree consumes it -- rom/kernel/kernel_base.h:34
 * claims kernel_arch.h "specifies IRQ_COUNT", but every user is spelled
 * HW_IRQ_COUNT, which we do not define, so kb_Interrupts[] is sized by the
 * fallback (256 - INTB_KERNEL = 240). Our 72 is the BCM283x legacy
 * controller's count, copied along with the rest from arm-native/kernel, and it
 * is inert in both. Left in place because it is the correct number and removing
 * it would only make the next reader wonder; noted here so nobody moves this
 * file on the theory that the kernel depends on a SoC constant.
 */
#include <inttypes.h>

#define IRQ_COUNT 72

extern void ictl_enable_irq(uint8_t irq, struct KernelBase *KernelBase);
extern void ictl_disable_irq(uint8_t irq, struct KernelBase *KernelBase);
