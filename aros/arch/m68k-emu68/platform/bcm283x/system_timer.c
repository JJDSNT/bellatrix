/*
 * Real Raspberry Pi (BCM283x) System Timer driver.
 *
 * Matched via FDT compatible="brcm,bcm2835-system-timer" under /soc; the
 * register layout is the real, documented BCM283x peripheral -- the same
 * physical device arch/aarch64-native/kernel/platform_bcm2708.c programs
 * on bare hardware. Compare channel 3 is used, mirroring that driver's own
 * VBLANK_TIMER choice: channels 0 and 2 are reserved for VideoCore
 * firmware use on real Raspberry Pi boards.
 */
#include "../platform.h"
#include "../../boot/boot.h"

#include <aros/kernel.h>
#include <aros/macros.h>
#include <exec/types.h>
#include <hardware/intbits.h>

#include <kernel_base.h>
#include <kernel_intr.h>
#include <kernel_interrupts.h>
#include <proto/kernel.h>

#include "exec_platform.h"

#define SYSTIMER_CS   0x00
#define SYSTIMER_CLO  0x04
#define SYSTIMER_CHI  0x08
#define SYSTIMER_C0   0x0c

#define SYSTIMER_CHANNEL 3
#define SYSTIMER_IRQ      3    /* logical IRQ: bank 0 (GPU), bit 3 */

/*
 * Microseconds of headroom a freshly programmed compare must still have when
 * the write lands. The comparator matches on equality against the 1 MHz free
 * running counter, so a target that CLO has already passed is not "late" --
 * it never matches again until the 32-bit counter wraps, 71 minutes later.
 * That silently stops the heartbeat, and with it every Wait() timeout,
 * Delay() and scheduler quantum, wherever the boot happened to be.
 *
 * Sized for the slowest thing that can sit between the write and the readback
 * -- an MMIO pair under emulation -- not for real silicon, where it would be
 * wildly generous. It is still only 1.3% of the 20 ms period, so overshooting
 * costs nothing measurable; undershooting costs the whole heartbeat.
 */
#define SYSTIMER_MIN_MARGIN 256

static ULONG systimer_base;
static ULONG systimer_interval_us;

/* The compare value currently programmed into the channel. */
static ULONG systimer_next;

/* Bring-up counter: how often the compare had to be re-armed because the
 * counter had already passed the intended target. Nonzero means the machine
 * came within a hair of losing the heartbeat outright. */
volatile ULONG emu68_platform_late = 0;

/* Kept for arch/m68k-emu68/boot/selftest.c, which polls this to confirm
 * the platform timer is actually ticking before trusting timer.device. */
volatile ULONG emu68_platform_ticks = 0;

/*
 * BCM283x registers are little-endian; the m68k guest is big-endian, and
 * Emu68 maps the peripheral block straight through without swapping. Every
 * 32-bit access therefore has to convert explicitly.
 *
 * arch/aarch64-native gets away with a plain dereference for the same
 * silicon only because ARM runs little-endian there. Reading a register
 * raw here yields a byte-reversed value, and -- worse -- writing one raw
 * still reads back "correctly" because both directions are reversed, so a
 * read-back check cannot detect the bug.
 */
static inline ULONG systimer_read(ULONG offset)
{
    return AROS_LE2LONG(*(volatile ULONG *)(systimer_base + offset));
}

static inline void systimer_write(ULONG offset, ULONG value)
{
    *(volatile ULONG *)(systimer_base + offset) = AROS_LONG2LE(value);
}

/*
 * Program the channel and make sure the target really is in the future when
 * the write lands. arch/aarch64-native/kernel/platform_bcm2708.c computes
 * CLO + interval and writes it once, which is safe on real hardware, where
 * the window between the read and the write is nanoseconds. Under Emu68 the
 * same window covers a JIT translation and whatever the serial console is
 * doing, so it can and does exceed a whole 20 ms period.
 *
 * Bounded so a pathologically slow environment cannot spin here forever.
 */
static void systimer_arm(ULONG target)
{
    int retries;

    for (retries = 0; retries < 8; retries++)
    {
        systimer_write(SYSTIMER_C0 + SYSTIMER_CHANNEL * 4, target);

        /* Signed difference, so this stays correct across the 32-bit wrap. */
        if ((LONG)(target - systimer_read(SYSTIMER_CLO)) > 0)
            break;

        target = systimer_read(SYSTIMER_CLO) + SYSTIMER_MIN_MARGIN;
        emu68_platform_late++;
    }

    systimer_next = target;
}

static void systimer_heartbeat(void *unused, void *unused2)
{
    ULONG now;

    (void)unused;
    (void)unused2;

    systimer_write(SYSTIMER_CS, 1UL << SYSTIMER_CHANNEL);
    now = systimer_read(SYSTIMER_CLO);
    emu68_platform_ticks++;
    emu68_bootui_clock_tick(now);

    /*
     * Advance from the compare that just fired rather than from CLO, so a
     * late interrupt does not stretch the period; systimer_arm() catches up
     * if we have fallen behind by more than one whole interval.
     */
    systimer_arm(systimer_next + systimer_interval_us);

    /* Bring-up trace: the first few ticks prove the whole delivery chain --
     * compare match -> interrupt controller -> Emu68's EXTER bridge ->
     * level-6 autovector -> dispatch -> here. Bounded so it cannot flood. */
    if ((emu68_platform_ticks & (emu68_platform_ticks - 1)) == 0 &&
        emu68_platform_ticks <= 0x10000)
        platform_trace_val("[systimer] IRQ tick ", emu68_platform_ticks);

    if (SysBase && (IDNESTCOUNT_GET < 0))
        core_Cause(INTB_VERTB, 1L << INTB_VERTB);
}

static BOOL systimer_init(const struct PlatformNode *node)
{
    systimer_base = node->base;
    if (KrnAddIRQHandler(SYSTIMER_IRQ, systimer_heartbeat, NULL, NULL) == NULL)
        return FALSE;
    emu68_bootui_clock_start(systimer_read(SYSTIMER_CLO));
    return TRUE;
}

static void systimer_set_period(ULONG interval_us)
{
    systimer_interval_us = interval_us;
}

static void systimer_enable(void)
{
    ULONG now = systimer_read(SYSTIMER_CLO);
    ULONG target = now + systimer_interval_us;

    emu68_platform_ticks = 0;
    emu68_platform_late = 0;
    systimer_arm(target);
    target = systimer_next;

    /* Bring-up trace: prove what the driver actually sees and programs,
     * rather than inferring it from the host side. */
    platform_trace_val("[systimer] base    ", systimer_base);
    platform_trace_val("[systimer] interval", systimer_interval_us);
    platform_trace_val("[systimer] CLO     ", now);
    platform_trace_val("[systimer] C3 want ", target);
    platform_trace_val("[systimer] C3 got  ",
                       systimer_read(SYSTIMER_C0 + SYSTIMER_CHANNEL * 4));
    platform_trace_val("[systimer] CLO now ", systimer_read(SYSTIMER_CLO));
    platform_trace_val("[systimer] CS      ", systimer_read(SYSTIMER_CS));

    /*
     * Watch the compare in isolation, before anything downstream can
     * confuse the picture: does the status bit ever set once CLO passes the
     * programmed target? This is pure MMIO polling -- no interrupt
     * controller, no EXTER bridge, no scheduler involved. If CLO passes
     * `target` and CS stays clear, the fault is in the peripheral or in how
     * we program it, and everything downstream is irrelevant.
     */
    {
        ULONG spins;
        ULONG cs = 0;
        ULONG clo = now;

        for (spins = 0; spins < 20000000UL; spins++)
        {
            cs = systimer_read(SYSTIMER_CS);
            clo = systimer_read(SYSTIMER_CLO);
            if (cs & (1UL << SYSTIMER_CHANNEL))
                break;
        }

        platform_trace_val("[systimer] poll CS ", cs);
        platform_trace_val("[systimer] poll CLO", clo);
        platform_trace(clo >= target
            ? "[systimer] CLO passed target\n"
            : "[systimer] CLO did NOT reach target\n");
        /* The INTREQR probe that used to sit here is gone with the Paula
         * shadow: interrupts arrive as a level in INTF.IPL now, and there is
         * no guest-visible register that says whether Emu68 saw the line.
         * Reading 0xdff01e would just read RAM and look like "no interrupt". */
        platform_trace_val("[intc] ENBL0 (raw) ",
                           *(volatile ULONG *)0xf200b210);
        platform_trace_val("[intc] PEND0 (raw) ",
                           *(volatile ULONG *)0xf200b204);

        platform_trace((cs & (1UL << SYSTIMER_CHANNEL))
            ? "[systimer] compare MATCHED\n"
            : "[systimer] compare never matched\n");
    }
}

static void systimer_disable(void)
{
    /* Masking the IRQ at the controller (ictl_disable_irq) is enough to
     * stop delivery; the compare register can be left alone. */
}

const struct PlatformTimerOps bcm283x_system_timer_ops = {
    systimer_init,
    systimer_set_period,
    systimer_enable,
    systimer_disable,
};

const struct PlatformDriver bcm283x_system_timer_driver = {
    "brcm,bcm2835-system-timer",
    &bcm283x_system_timer_ops,
    NULL,
};
