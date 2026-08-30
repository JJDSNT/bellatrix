/*
 * Copyright (C) 2026, The Bellatrix Project.
 *
 * DWC2 controller lifecycle. All functions except the tiny IRQ top half run
 * in the unit task and therefore own the controller state without list locks.
 */

#include <aros/debug.h>

#include <devices/timer.h>
#include <hardware/bcm2708.h>

#include <proto/exec.h>

#include "dwc2emu68_intern.h"
#include "dwc2emu68_regs.h"

#define KernelBase device->kernel_base
#include <proto/kernel.h>

#define DWC2_WAIT_LOOPS 100
#define DWC2_HALT_WAIT_LOOPS 100000

AROS_INTH1(DWC2_FNAME(SoftIRQ), struct DWC2Unit *, unit)
{
    AROS_INTFUNC_INIT

    Signal(unit->task, 1UL << unit->port->mp_SigBit);
    return FALSE;

    AROS_INTFUNC_EXIT
}

BOOL dwc2_delay_us(struct DWC2Unit *unit, ULONG microseconds)
{
    struct timerequest *request = unit->timer_request;

    if (request == NULL)
        return FALSE;
    request->tr_node.io_Command = TR_ADDREQUEST;
    request->tr_time.tv_secs = microseconds / 1000000;
    request->tr_time.tv_micro = microseconds % 1000000;
    DoIO((struct IORequest *)request);
    return request->tr_node.io_Error == 0;
}

static BOOL wait_set(struct DWC2Device *device, ULONG reg, ULONG bits)
{
    ULONG count;

    for (count = 0; count < DWC2_WAIT_LOOPS; count++)
        if ((dwc2_readl(device, reg) & bits) == bits)
            return TRUE;
    return FALSE;
}

static BOOL wait_clear(struct DWC2Device *device, ULONG reg, ULONG bits)
{
    ULONG count;

    for (count = 0; count < DWC2_WAIT_LOOPS; count++)
        if ((dwc2_readl(device, reg) & bits) == 0)
            return TRUE;
    return FALSE;
}

static ULONG hprt_write_value(ULONG hprt)
{
    /* PRTENA is status on read but write-one-to-disable on write. */
    return hprt & ~(DWC2_HPRT_CHANGE_BITS | DWC2_HPRT_ENA);
}

static void controller_irq(void *data, void *sysbase)
{
    struct DWC2Unit *unit = data;
    struct DWC2Device *device = unit->device;
    ULONG active = dwc2_readl(device, DWC2_GINTSTS) &
        dwc2_readl(device, DWC2_GINTMSK);
    ULONG ahb;

    (void)sysbase;
    if (active == 0)
        return;

    if (active & DWC2_GINTSTS_SOF)
    {
        ULONG now = (dwc2_readl(device, DWC2_HFNUM) & 0x3fffUL) >> 3;
        ULONG due = unit->periodic_due & 0x07ffUL;

        dwc2_writel(device, DWC2_GINTSTS, DWC2_GINTSTS_SOF);
        /*
         * SOF is suppressed unless something is actually waiting on this
         * frame, because delivering every one of them is a storm nobody
         * reads. Two things wait on it, and only one used to be counted: a
         * periodic transfer that has come due, and a split transaction
         * pacing its complete-split.
         *
         * Missing the second is silent and total. The start-split is
         * accepted, the sequencer schedules the complete-split for a few
         * microframes later, and the countdown that would issue it lives in
         * the SOF handler -- which never runs, because no periodic transfer
         * happens to be queued. Every device behind a hub then dies in the
         * watchdog with its channel still armed, and enumeration cannot get
         * past the first one.
         */
        if (unit->split_pacing != 0)
            dwc2_transfer_split_sof(unit);
        if (!unit->periodic_waiting ||
            ((now - due) & 0x07ffUL) >= 0x0400UL)
            active &= ~DWC2_GINTSTS_SOF;
    }
    if (active == 0)
        return;

    /* Gate the controller before waking the owner. No queues, allocation,
     * cache operation or transfer completion is permitted in this top half. */
    ahb = dwc2_readl(device, DWC2_GAHBCFG);
    dwc2_writel(device, DWC2_GAHBCFG,
        ahb & ~DWC2_GAHBCFG_GLBLINTRMSK);
    if (active & DWC2_GINTSTS_HCHINT)
    {
        /*
         * HAINT names the channels that raised an interrupt, one bit each.
         * Take each one's HCINT, acknowledge it here so the level-triggered
         * source drops, and hand the bits to the unit task through the
         * channel that produced them. Reading HCINT(0) unconditionally --
         * which is what this did while there was only one channel -- both
         * misses every other channel and attributes their state to channel 0.
         */
        ULONG flagged = dwc2_readl(device, DWC2_HAINT) &
            dwc2_readl(device, DWC2_HAINTMSK);
        UBYTE i;

        for (i = 0; i < unit->host_channels; i++)
        {
            ULONG channel_status;

            if (!(flagged & (1UL << i)))
                continue;
            channel_status = dwc2_readl(device, DWC2_HCINT(i));
            if (channel_status == 0)
                continue;
            dwc2_writel(device, DWC2_HCINT(i), channel_status);
            /* A split's next step has a deadline the unit task cannot meet,
             * so it is taken here. When this claims the interrupt there is
             * nothing left for the task to be woken for. */
            if (dwc2_transfer_split_irq(unit, i, channel_status))
                continue;
            unit->channel[i].pending |= channel_status;
            unit->channels_pending |= 1UL << i;
        }
        dwc2_writel(device, DWC2_GINTSTS, DWC2_GINTSTS_HCHINT);
    }
    unit->irq_pending |= active;
    Cause(&unit->soft_irq);
}

/*
 * Set the frame interval from the speed the root port negotiated.
 *
 * HFIR.FRINT counts PHY clocks per (micro)frame: a millisecond of them at
 * full and low speed, an eighth of that at high speed. The rate itself comes
 * from how the core is strapped -- a UTMI+ at eight bits wide runs at 60 MHz
 * and at sixteen bits at half that for the same throughput, a core wired to
 * the USB 1.1 serial transceiver runs at 48 MHz, and PHYLPCS raises a
 * sixteen-bit interface back to 48 MHz.
 *
 * Getting this wrong is not merely inaccurate. The core decides whether a
 * transaction still fits in the time left in the frame before it starts one,
 * so a frame interval eight times too long tells it there is eight times more
 * room than there is. Most transactions still land; the ones that start near
 * a real boundary are cut short and come back as transaction errors. That is
 * an intermittent fault with no obvious cause, which is the expensive kind.
 */
void dwc2_controller_speed(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;
    ULONG gusbcfg = dwc2_readl(device, DWC2_GUSBCFG);
    ULONG clock_khz;
    ULONG frint;

    if (gusbcfg & DWC2_GUSBCFG_PHYSEL_FS)
        clock_khz = 48000;
    else if (gusbcfg & DWC2_GUSBCFG_PHYIF16)
        clock_khz = (gusbcfg & DWC2_GUSBCFG_PHYLPCS) ? 48000 : 30000;
    else
        clock_khz = 60000;

    frint = ((dwc2_readl(device, DWC2_HPRT) & DWC2_HPRT_SPD_MASK) ==
        DWC2_HPRT_SPD_HIGH) ? clock_khz / 8 : clock_khz;
    dwc2_writel(device, DWC2_HFIR,
        (dwc2_readl(device, DWC2_HFIR) & ~DWC2_HFIR_FRINT_MASK) | frint);
}

BOOL dwc2_controller_start(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;
    ULONG value;
    ULONG channel;
    ULONG count;
    ULONG active_channels = 0;

    if (unit->initialized)
        return unit->hardware_ok;
    unit->initialized = TRUE;

    bug("[DWC2/Emu68] starting host controller\n");

    unit->saved_gusbcfg = dwc2_readl(device, DWC2_GUSBCFG);
    dwc2_writel(device, DWC2_PCGCCTL, 0);
    dwc2_writel(device, DWC2_GINTMSK, 0);
    value = dwc2_readl(device, DWC2_GAHBCFG);
    value &= ~(DWC2_GAHBCFG_GLBLINTRMSK | DWC2_GAHBCFG_DMAEN);
    dwc2_writel(device, DWC2_GAHBCFG, value);

    if (!wait_set(device, DWC2_GRSTCTL, DWC2_GRSTCTL_AHBIDLE))
        goto failed;
    bug("[DWC2/Emu68] AHB idle, resetting core\n");
    dwc2_writel(device, DWC2_GRSTCTL, DWC2_GRSTCTL_CSFTRST);
    if (!wait_clear(device, DWC2_GRSTCTL, DWC2_GRSTCTL_CSFTRST))
        goto failed;
    bug("[DWC2/Emu68] core reset complete\n");
    if (!dwc2_delay_us(unit, 1000))
        goto failed;
    bug("[DWC2/Emu68] reset settle complete\n");

    value = unit->saved_gusbcfg & ~DWC2_GUSBCFG_FORCEDEVMODE;
    dwc2_writel(device, DWC2_GUSBCFG, value | DWC2_GUSBCFG_FORCEHOSTMODE);
    if (!dwc2_delay_us(unit, 25000))
        goto failed;
    bug("[DWC2/Emu68] host-mode settle complete\n");

    value = dwc2_readl(device, DWC2_HCFG) & ~3UL;
    dwc2_writel(device, DWC2_HCFG, value);
    /* HFIR is left at the core's reset default here on purpose: the frame
     * interval depends on the speed the port negotiates, and nothing has
     * negotiated yet. dwc2_controller_speed() sets it once there is an
     * answer, which is before anything is armed against the port. */

    dwc2_writel(device, DWC2_GRXFSIZ, 774);
    dwc2_writel(device, DWC2_GNPTXFSIZ, (256UL << 16) | 774);
    dwc2_writel(device, DWC2_HPTXFSIZ, (512UL << 16) | 1030);
    dwc2_writel(device, DWC2_GRSTCTL,
        DWC2_GRSTCTL_TXFFLSH | DWC2_GRSTCTL_TXFNUM_ALL);
    if (!wait_clear(device, DWC2_GRSTCTL, DWC2_GRSTCTL_TXFFLSH))
        goto failed;
    dwc2_writel(device, DWC2_GRSTCTL, DWC2_GRSTCTL_RXFFLSH);
    if (!wait_clear(device, DWC2_GRSTCTL, DWC2_GRSTCTL_RXFFLSH))
        goto failed;
    bug("[DWC2/Emu68] FIFOs configured and flushed\n");

    unit->host_channels = DWC2_GHWCFG2_NUM_HOST_CHAN(
        dwc2_readl(device, DWC2_GHWCFG2));
    /* The ceiling is the size of unit->channel[], not an architectural limit:
     * a DWC2 may report up to sixteen, and indexing past the array would be
     * worse than driving fewer channels than the core has. BCM283x reports
     * eight, which is what DWC2_MAX_CHANNELS is sized for. */
    if (unit->host_channels > DWC2_MAX_CHANNELS)
        unit->host_channels = DWC2_MAX_CHANNELS;
    /* Synopsys specifies a two-phase shutdown. First publish CHDIS with
     * CHENA clear for every channel; only then request the forced halt. The
     * BCM2837 core needs substantially longer than QEMU to retire that
     * request, so this path has its own bounded wait. */
    for (channel = 0; channel < unit->host_channels; channel++)
    {
        value = dwc2_readl(device, DWC2_HCCHAR(channel));
        if (value & DWC2_HCCHAR_CHENA)
        {
            active_channels |= 1UL << channel;
            value &= ~(DWC2_HCCHAR_CHENA | DWC2_HCCHAR_EPDIR_IN);
            value |= DWC2_HCCHAR_CHDIS;
        }
        else
            value &= ~(DWC2_HCCHAR_CHENA | DWC2_HCCHAR_CHDIS |
                DWC2_HCCHAR_EPDIR_IN);
        dwc2_writel(device, DWC2_HCCHAR(channel), value);
    }
    for (channel = 0; channel < unit->host_channels; channel++)
    {
        if (!(active_channels & (1UL << channel)))
        {
            dwc2_writel(device, DWC2_HCINT(channel), DWC2_HCINT_ALL);
            dwc2_writel(device, DWC2_HCINTMSK(channel), 0);
            continue;
        }
        value = dwc2_readl(device, DWC2_HCCHAR(channel));
        value &= ~DWC2_HCCHAR_EPDIR_IN;
        value |= DWC2_HCCHAR_CHDIS | DWC2_HCCHAR_CHENA;
        dwc2_writel(device, DWC2_HCCHAR(channel), value);
        for (count = 0; count < DWC2_HALT_WAIT_LOOPS; count++)
            if (!(dwc2_readl(device, DWC2_HCCHAR(channel)) &
                DWC2_HCCHAR_CHENA))
                break;
        if (count == DWC2_HALT_WAIT_LOOPS)
        {
            bug("[DWC2/Emu68] channel %lu halt timed out HCCHAR=%08lx\n",
                channel, dwc2_readl(device, DWC2_HCCHAR(channel)));
            goto failed;
        }
        dwc2_writel(device, DWC2_HCINT(channel), DWC2_HCINT_ALL);
        dwc2_writel(device, DWC2_HCINTMSK(channel), 0);
    }
    dwc2_writel(device, DWC2_HAINTMSK, 0);
    bug("[DWC2/Emu68] %lu channels quiesced\n",
        (ULONG)unit->host_channels);

    value = dwc2_readl(device, DWC2_HPRT);
    if (!(value & DWC2_HPRT_PWR))
        dwc2_writel(device, DWC2_HPRT, hprt_write_value(value) | DWC2_HPRT_PWR);
    bug("[DWC2/Emu68] root port powered, HPRT=%08lx\n",
        dwc2_readl(device, DWC2_HPRT));
    if (dwc2_readl(device, DWC2_HPRT) & DWC2_HPRT_CHANGE_BITS)
        unit->port_changed = TRUE;

    bug("[DWC2/Emu68] registering IRQ %lu\n", (ULONG)IRQ_VC_USB);
    unit->irq_handle = KrnAddIRQHandler(IRQ_VC_USB, controller_irq, unit, NULL);
    if (unit->irq_handle == NULL)
        goto failed;
    bug("[DWC2/Emu68] IRQ registered\n");

    /*
     * No source is enabled until the unit task has a consumer for it.
     *
     * The line below used to say that and then unmask SOF anyway, which is
     * the one source with no consumer at this point and the one that never
     * stops: 8 kHz, from here to the end of the machine's life. On a Pi that
     * is an ARM handler; here it is an m68k level-6 exception through the
     * JIT, and the CPU cannot get out from under it -- it spins in
     * supervisor mode at IPL 6, no task is ever scheduled again, and the boot
     * clock stops at the second this line runs while every other core carries
     * on logging.
     *
     * Masking it in set_sof_irq() was not enough because this write does not
     * go through set_sof_irq(). arm() enables HCHINT when it needs it, and
     * update_sof_irq() asks for SOF for the one frame a periodic transfer is
     * due in. Nothing needs anything before that.
     *
     * QEMU is the other half of the same rule: it starts with HPRT.CONNDET
     * asserted, so enabling PRTINT here would storm before the root hub is
     * online.
     */
    dwc2_writel(device, DWC2_GINTSTS, 0xffffffffUL);
    dwc2_writel(device, DWC2_GINTMSK, 0);
    value = dwc2_readl(device, DWC2_GAHBCFG);
    value |= DWC2_GAHBCFG_DMAEN | DWC2_GAHBCFG_HBSTLEN_INCR |
        DWC2_GAHBCFG_NPTXFEMPLVL | DWC2_GAHBCFG_GLBLINTRMSK;
    dwc2_writel(device, DWC2_GAHBCFG, value);

    bug("[DWC2/Emu68] config AHB=%08lx USB=%08lx HW2=%08lx HW3=%08lx "
        "HCFG=%08lx HFIR=%08lx\n",
        dwc2_readl(device, DWC2_GAHBCFG),
        dwc2_readl(device, DWC2_GUSBCFG),
        dwc2_readl(device, DWC2_GHWCFG2),
        dwc2_readl(device, DWC2_GHWCFG3),
        dwc2_readl(device, DWC2_HCFG),
        dwc2_readl(device, DWC2_HFIR));
    dwc2_platform_log_clocks(device);

    unit->hardware_ok = TRUE;
    bug("[DWC2/Emu68] host initialized with %lu channels, no source unmasked\n",
        (ULONG)unit->host_channels);
    return TRUE;

failed:
    dwc2_writel(device, DWC2_GINTMSK, 0);
    bug("[DWC2/Emu68] controller initialization failed\n");
    return FALSE;
}

void dwc2_controller_drain_irq(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;
    ULONG pending = unit->irq_pending;
    ULONG value;

    unit->irq_pending = 0;
    if (pending & (DWC2_GINTSTS_PRTINT | DWC2_GINTSTS_DISCONNINT))
    {
        value = dwc2_readl(device, DWC2_HPRT);
        if (value & DWC2_HPRT_CHANGE_BITS)
        {
            unit->port_changed = TRUE;
            dwc2_writel(device, DWC2_HPRT,
                hprt_write_value(value) | (value & DWC2_HPRT_CHANGE_BITS));
        }
        if (pending & DWC2_GINTSTS_DISCONNINT)
            dwc2_writel(device, DWC2_GINTSTS, DWC2_GINTSTS_DISCONNINT);
    }
    if (pending & DWC2_GINTSTS_HCHINT)
        dwc2_transfer_irq(unit);
    if (pending & DWC2_GINTSTS_SOF)
        dwc2_transfer_sof(unit);

    value = dwc2_readl(device, DWC2_GAHBCFG);
    dwc2_writel(device, DWC2_GAHBCFG,
        value | DWC2_GAHBCFG_GLBLINTRMSK);
    dwc2_root_poll(unit);
}
