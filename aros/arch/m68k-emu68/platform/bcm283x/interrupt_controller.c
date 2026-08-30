/*
 * Real Raspberry Pi (BCM283x) legacy interrupt controller driver.
 *
 * Matched via FDT compatible="brcm,bcm2836-armctrl-ic" under /soc. Decodes
 * the aggregated pending banks (ARMIRQ_PEND/GPUIRQ_PEND0/GPUIRQ_PEND1)
 * exactly the way arch/aarch64-native/kernel/platform_bcm2708.c's
 * bcm2708_irq_process() does on bare hardware. Under Emu68 this is reached
 * from the m68k level-6 autovector (Emu68's fixed "EXTER" channel for any
 * real physical IRQ not claimed by one of its own virtual devices) instead
 * of an ARM64 vector table entry, but decoding which real source fired is
 * identical -- same silicon, same registers.
 *
 * Logical IRQ numbering matches
 * arch/arm-native/soc/broadcom/2708/include/hardware/bcm2708.h:
 * bank = irq >> 5, bit = irq & 0x1f. Bank 0 = GPUIRQ_PEND0/ENBL0/DIBL0,
 * bank 1 = GPUIRQ_PEND1/ENBL1/DIBL1, bank 2 = the small ARM-side set in
 * ARMIRQ_PEND/ENBL/DIBL.
 */
#include "../platform.h"

#include <aros/kernel.h>
#include <aros/macros.h>
#include <exec/types.h>

#include <kernel_base.h>
#include <kernel_interrupts.h>
#include <proto/kernel.h>

#define ARMIRQ_PEND   0x00
#define GPUIRQ_PEND0  0x04
#define GPUIRQ_PEND1  0x08
#define ARMFIQ_CTRL   0x0c
#define GPUIRQ_ENBL0  0x10
#define GPUIRQ_ENBL1  0x14
#define ARMIRQ_ENBL   0x18
#define GPUIRQ_DIBL0  0x1c
#define GPUIRQ_DIBL1  0x20
#define ARMIRQ_DIBL   0x24

/*
 * ARMIRQ_PEND only has eight real sources, in bits 0-7. Everything above
 * that is a mirror and must never be dispatched:
 *
 *   bits 8,9   - "something is pending in bank 0 / bank 1"
 *   bits 10-20 - duplicates of GPU IRQs 7, 9, 10, 18, 19, 53, 54, 55, 56,
 *                57 and 62, provided so software can poll the common ones
 *                without reading the GPU banks.
 *
 * Dispatching a mirror invents an IRQ number nobody registered a handler
 * for, so the source is never acknowledged, stays pending, and the loop in
 * intc_dispatch() spins forever inside the level-6 handler. Bit 20 mirrors
 * GPU IRQ 62, the Arasan SD controller -- which is exactly how this was
 * found. arch/arm-native and arch/aarch64-native avoid it by scanning only
 * eight bits of this bank; so do we.
 *
 * The same trap exists one level down, for the real bit rather than its
 * mirror: a source with no handler is never acknowledged whatever bank it
 * fired in. GPU IRQ 62 reaches GPUIRQ_PEND1 bit 30 as soon as the SD driver
 * writes SDHCI_SIGNAL_ENABLE, and nothing on this port registers a handler
 * for it -- the driver polls INT_STATUS instead. So every pending bank is
 * masked with its enable register before dispatch: what a driver has not
 * unmasked through KrnAddIRQHandler() is not ours to acknowledge.
 */
#define ARMIRQ_REAL_BITS 8

#define IRQ_BANK(irq)  ((irq) >> 5)
#define IRQ_MASK(irq)  (1UL << ((irq) & 0x1f))

static ULONG intc_base;

/*
 * BCM283x registers are little-endian and the m68k guest is big-endian;
 * Emu68 maps the peripheral block through unswapped, so every 32-bit
 * access has to convert explicitly. See the equivalent note in
 * system_timer.c -- a raw write here lands byte-reversed, which turns
 * "unmask IRQ 3" into "unmask IRQ 27".
 */
static inline ULONG intc_read(ULONG offset)
{
    return AROS_LE2LONG(*(volatile ULONG *)(intc_base + offset));
}

static inline void intc_write(ULONG offset, ULONG value)
{
    *(volatile ULONG *)(intc_base + offset) = AROS_LONG2LE(value);
}

static BOOL intc_init(const struct PlatformNode *node)
{
    intc_base = node->base;

    /* Make sure no source is stolen by the FIQ path. */
    intc_write(ARMFIQ_CTRL, 0);

    /* Start fully masked; individual drivers unmask their own IRQ as they
     * call KrnAddIRQHandler() (see ictl_enable_irq() in platform.c). */
    intc_write(GPUIRQ_DIBL0, ~0UL);
    intc_write(GPUIRQ_DIBL1, ~0UL);
    intc_write(ARMIRQ_DIBL, ~0UL);

    return TRUE;
}

static ULONG bank_enable_offset(ULONG bank)
{
    if (bank == 0)
        return GPUIRQ_ENBL0;
    if (bank == 1)
        return GPUIRQ_ENBL1;
    return ARMIRQ_ENBL;
}

static ULONG bank_disable_offset(ULONG bank)
{
    if (bank == 0)
        return GPUIRQ_DIBL0;
    if (bank == 1)
        return GPUIRQ_DIBL1;
    return ARMIRQ_DIBL;
}

static void intc_enable(ULONG irq)
{
    intc_write(bank_enable_offset(IRQ_BANK(irq)), IRQ_MASK(irq));
}

static void intc_disable(ULONG irq)
{
    intc_write(bank_disable_offset(IRQ_BANK(irq)), IRQ_MASK(irq));
}

static void scan_bank(struct KernelBase *KernelBase, ULONG pending, ULONG base,
                      ULONG bits)
{
    ULONG bit;

    for (bit = 0; bit < bits; bit++)
    {
        if (pending & (1UL << bit))
            krnRunIRQHandlers(KernelBase, base + bit);
    }
}

/*
 * How many times one entry may drain the controller before returning.
 *
 * This was `for (;;)` -- keep going until nothing is pending -- which is
 * correct only if every source can be served faster than it re-asserts. The
 * DWC2 start-of-frame interrupt cannot: it arrives every 125 us for as long as
 * a periodic transfer is queued, and the loop below reads five controller
 * registers and runs a handler per round. The loop then never reaches a quiet
 * read, the m68k never leaves the level-6 exception, no task is ever
 * scheduled again, and the machine looks exactly like a hang: the boot clock
 * stops, and every other core keeps logging.
 *
 * That is what a chipset core reporting from outside the CPU showed --
 * `[BELLATRIX:LIVE] arm=0 ipl=0 sr=2610` with SR still changing, so a CPU
 * spinning in supervisor mode at IPL 6, not one parked on a STOP.
 *
 * It was unreachable until the mouse's interrupt pipe started running: no
 * periodic transfer ever reached the host controller before, so SOF was never
 * enabled and this loop never met a source it could not outrun.
 *
 * A bound is the whole fix. The interrupt is a level, not an edge: whatever
 * is still pending re-enters immediately, and re-entering is what the
 * autovector is for. Draining a few rounds per entry keeps the saving the
 * loop was written for -- most entries have one source and finish in one
 * round -- without promising to outrun the hardware.
 */
#define INTC_DRAIN_ROUNDS 4

static void intc_dispatch(struct KernelBase *KernelBase)
{
    ULONG pending_arm, pending0, pending1;
    unsigned round;

    for (round = 0; round < INTC_DRAIN_ROUNDS; round++)
    {
        pending_arm = intc_read(ARMIRQ_PEND) &
                      intc_read(ARMIRQ_ENBL) &
                      ((1UL << ARMIRQ_REAL_BITS) - 1);
        pending0 = intc_read(GPUIRQ_PEND0) & intc_read(GPUIRQ_ENBL0);
        pending1 = intc_read(GPUIRQ_PEND1) & intc_read(GPUIRQ_ENBL1);

        if (!(pending_arm || pending0 || pending1))
            break;

        if (pending_arm)
            scan_bank(KernelBase, pending_arm, 2 << 5, ARMIRQ_REAL_BITS);
        if (pending0)
            scan_bank(KernelBase, pending0, 0 << 5, 32);
        if (pending1)
            scan_bank(KernelBase, pending1, 1 << 5, 32);
    }
}

const struct PlatformIntcOps bcm283x_armctrl_ic_ops = {
    intc_init,
    intc_enable,
    intc_disable,
    intc_dispatch,
};

const struct PlatformDriver bcm283x_armctrl_ic_driver = {
    "brcm,bcm2836-armctrl-ic",
    NULL,
    &bcm283x_armctrl_ic_ops,
};
