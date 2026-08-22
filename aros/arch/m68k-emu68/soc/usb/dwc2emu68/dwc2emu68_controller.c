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
#define DWC2_MAX_CHANNELS 16

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
        ULONG channel_status = dwc2_readl(device, DWC2_HCINT(0));

        unit->channel_pending |= channel_status;
        dwc2_writel(device, DWC2_HCINT(0), channel_status);
        dwc2_writel(device, DWC2_GINTSTS, DWC2_GINTSTS_HCHINT);
    }
    unit->irq_pending |= active;
    Cause(&unit->soft_irq);
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
    dwc2_writel(device, DWC2_HFIR,
        (dwc2_readl(device, DWC2_HFIR) & ~0xffffUL) | 60000);

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

    /* No source is enabled until the unit task has a consumer for it. In
     * particular QEMU starts with HPRT.CONNDET asserted; enabling PRTINT here
     * would create a guest interrupt storm before the root hub is online. */
    dwc2_writel(device, DWC2_GINTSTS, 0xffffffffUL);
    dwc2_writel(device, DWC2_GINTMSK, DWC2_GINTSTS_SOF);
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
    bug("[DWC2/Emu68] host initialized with %lu channels, SOF enabled\n",
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
