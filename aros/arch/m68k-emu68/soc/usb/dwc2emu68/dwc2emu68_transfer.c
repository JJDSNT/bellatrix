/*
 * Copyright (C) 2026, The Bellatrix Project.
 *
 * Single-channel DWC2 transfer engine. The unit task is the sole owner; the
 * hardware IRQ only gates the controller and wakes that task.
 */

#include <aros/debug.h>

#include <exec/memory.h>
#include <proto/exec.h>

#include "dwc2emu68_intern.h"
#include "dwc2emu68_regs.h"

#define DWC2_CHANNEL 0
#define STAGE_SETUP  1
#define STAGE_DATA   2
#define STAGE_STATUS 3
#define STAGE_INT_IN 4

static void start_next(struct DWC2Unit *unit);

#define DWC2_FRAME_MASK 0x07ffUL
#define DWC2_CONTROL_GATE_LOOPS 8000000UL

static ULONG current_frame(struct DWC2Unit *unit)
{
    return (dwc2_readl(unit->device, DWC2_HFNUM) & 0x3fffUL) >> 3;
}

static BOOL frame_due(ULONG now, ULONG due)
{
    return ((now - due) & DWC2_FRAME_MASK) < 0x0400UL;
}

static void set_sof_irq(struct DWC2Unit *unit, BOOL enabled)
{
    struct DWC2Device *device = unit->device;
    ULONG mask = dwc2_readl(device, DWC2_GINTMSK);

    if (enabled)
        mask |= DWC2_GINTSTS_SOF;
    else
        mask &= ~DWC2_GINTSTS_SOF;
    dwc2_writel(device, DWC2_GINTMSK, mask);
}

static void update_sof_irq(struct DWC2Unit *unit)
{
    struct Node *node = unit->periodic_queue.lh_Head;
    ULONG now = current_frame(unit);
    ULONG best_delta = DWC2_FRAME_MASK + 1;
    ULONG best_due = 0;

    while (node->ln_Succ != NULL)
    {
        struct IOUsbHWReq *ioreq = (struct IOUsbHWReq *)node;
        ULONG due = (ULONG)(IPTR)ioreq->iouh_DriverPrivate1 &
            DWC2_FRAME_MASK;
        ULONG delta = (due - now) & DWC2_FRAME_MASK;

        if (frame_due(now, due))
            delta = 0;
        if (delta < best_delta)
        {
            best_delta = delta;
            best_due = due;
        }
        node = node->ln_Succ;
    }
    unit->periodic_due = best_due;
    unit->periodic_waiting = best_delta <= DWC2_FRAME_MASK;
    set_sof_irq(unit, unit->periodic_waiting);
}

static ULONG dma_address(const void *address)
{
    return 0xc0000000UL | (ULONG)(IPTR)address;
}

static void wait_control_window(struct DWC2Device *device)
{
    ULONG count = DWC2_CONTROL_GATE_LOOPS;
    ULONG microframe;

    /* BCM2837 can deschedule a non-split control channel when it is armed at
     * the frame boundary, reporting only CHHLTD and leaving HCTSIZ untouched.
     * Arm in the stable middle of a frame; completion-chained stages pass
     * through the same gate so SETUP/DATA/STATUS use one rule. */
    do
    {
        microframe = dwc2_readl(device, DWC2_HFNUM) & 7;
    } while ((microframe < 2 || microframe > 5) && --count != 0);
}

static BOOL reset_halted_channel(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;
    ULONG hcchar = dwc2_readl(device, DWC2_HCCHAR(DWC2_CHANNEL));
    ULONG count;

    /* A bare CHHLTD leaves the BCM2837 channel state machine unable to run a
     * later transaction. Drive one real disable cycle while the endpoint
     * context is still present, then scrub the channel before reprogramming. */
    dwc2_writel(device, DWC2_HCCHAR(DWC2_CHANNEL),
        hcchar | DWC2_HCCHAR_CHENA | DWC2_HCCHAR_CHDIS);
    for (count = 0; count < 200000; count++)
        if (!(dwc2_readl(device, DWC2_HCCHAR(DWC2_CHANNEL)) &
            DWC2_HCCHAR_CHENA))
            break;
    if (count == 200000)
    {
        bug("[DWC2/Emu68:XFER] channel recovery timed out HCCHAR=%08lx\n",
            dwc2_readl(device, DWC2_HCCHAR(DWC2_CHANNEL)));
        return FALSE;
    }
    dwc2_writel(device, DWC2_HCINT(DWC2_CHANNEL), DWC2_HCINT_ALL);
    dwc2_writel(device, DWC2_HCSPLT(DWC2_CHANNEL), 0);
    dwc2_writel(device, DWC2_HCCHAR(DWC2_CHANNEL), 0);
    return TRUE;
}

static void finish(struct DWC2Unit *unit, BYTE error)
{
    struct IOUsbHWReq *ioreq = unit->active_request;
    struct DWC2Device *device = unit->device;

    unit->active_request = NULL;
    unit->transfer_stage = 0;
    ioreq->iouh_DriverPrivate1 = NULL;
    dwc2_writel(device, DWC2_HAINTMSK, 0);
    dwc2_writel(device, DWC2_GINTMSK,
        dwc2_readl(device, DWC2_GINTMSK) & ~DWC2_GINTSTS_HCHINT);
    ioreq->iouh_Req.io_Error = error;
    ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
    if (!(ioreq->iouh_Req.io_Flags & IOF_QUICK))
        ReplyMsg(&ioreq->iouh_Req.io_Message);

    start_next(unit);
}

static void start_next(struct DWC2Unit *unit)
{
    struct IOUsbHWReq *next;

    while ((next = (struct IOUsbHWReq *)RemHead(
        &unit->transfer_queue)) != NULL)
    {
        BYTE error = next->iouh_DriverPrivate2 != NULL ?
            IOERR_ABORTED : UHIOERR_HOSTERROR;

        if (error != IOERR_ABORTED && dwc2_transfer_submit(unit, next))
            return;
        next->iouh_Req.io_Error = error;
        next->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
        ReplyMsg(&next->iouh_Req.io_Message);
    }
}

void dwc2_transfer_abort_active(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;
    ULONG hcchar;
    ULONG count;

    if (unit->active_request == NULL)
        return;
    hcchar = dwc2_readl(device, DWC2_HCCHAR(DWC2_CHANNEL));
    if (hcchar & DWC2_HCCHAR_CHENA)
    {
        dwc2_writel(device, DWC2_HCCHAR(DWC2_CHANNEL),
            hcchar | DWC2_HCCHAR_CHDIS | DWC2_HCCHAR_CHENA);
        for (count = 0; count < 100; count++)
            if (!(dwc2_readl(device, DWC2_HCCHAR(DWC2_CHANNEL)) &
                DWC2_HCCHAR_CHENA))
                break;
    }
    unit->channel_pending = 0;
    dwc2_writel(device, DWC2_HCINT(DWC2_CHANNEL), DWC2_HCINT_ALL);
    finish(unit, IOERR_ABORTED);
}

static BOOL arm(struct DWC2Unit *unit, BOOL input, ULONG endpoint_type,
    ULONG pid, ULONG length)
{
    struct IOUsbHWReq *ioreq = unit->active_request;
    struct DWC2Device *device = unit->device;
    ULONG mps = ioreq->iouh_MaxPktSize;
    ULONG packets;
    ULONG hcchar;
    ULONG mask;

    if (mps == 0 || length > unit->dma_length)
        return FALSE;
    packets = length == 0 ? 1 : (length + mps - 1) / mps;
    unit->active_length = length;

    if (input && length != 0)
        CacheClearE(unit->dma_buffer, length, CACRF_InvalidateD);
    else if (length != 0)
        CacheClearE(unit->dma_buffer, length, CACRF_ClearD);

    hcchar = DWC2_HCCHAR_DEVADDR(ioreq->iouh_DevAddr) |
        DWC2_HCCHAR_EPNUM(ioreq->iouh_Endpoint) |
        DWC2_HCCHAR_MPS(mps) | DWC2_HCCHAR_MULTICNT_ONE | endpoint_type;
    if (input)
        hcchar |= DWC2_HCCHAR_EPDIR_IN;
    if (endpoint_type == DWC2_HCCHAR_EPTYPE_INTERRUPT &&
        !(dwc2_readl(device, DWC2_HFNUM) & 1))
        hcchar |= DWC2_HCCHAR_ODDFRM;

    /* Program the endpoint characteristics while the channel is disabled.
     * The core latches this context before HCTSIZ/HCDMA; QEMU, like the
     * hardware, does not start a channel reliably when HCCHAR is written only
     * once with CHENA after the other registers. */
    dwc2_writel(device, DWC2_HCCHAR(DWC2_CHANNEL), hcchar);
    dwc2_writel(device, DWC2_HCSPLT(DWC2_CHANNEL), 0);
    dwc2_writel(device, DWC2_HCINT(DWC2_CHANNEL), DWC2_HCINT_ALL);
    mask = DWC2_HCINT_XFERCOMP | DWC2_HCINT_CHHLTD |
        DWC2_HCINT_AHBERR | DWC2_HCINT_STALL | DWC2_HCINT_NAK |
        DWC2_HCINT_NYET | DWC2_HCINT_XACTERR | DWC2_HCINT_BBLERR |
        DWC2_HCINT_DATATGLERR;
    dwc2_writel(device, DWC2_HCINTMSK(DWC2_CHANNEL), mask);
    dwc2_writel(device, DWC2_HCTSIZ(DWC2_CHANNEL),
        DWC2_HCTSIZ_XFERSIZE(length) | DWC2_HCTSIZ_PKTCNT(packets) | pid);
    dwc2_writel(device, DWC2_HCDMA(DWC2_CHANNEL),
        dma_address(unit->dma_buffer));
    dwc2_writel(device, DWC2_HAINTMSK, 1UL << DWC2_CHANNEL);
    dwc2_writel(device, DWC2_GINTMSK,
        dwc2_readl(device, DWC2_GINTMSK) | DWC2_GINTSTS_HCHINT);
    if (endpoint_type == DWC2_HCCHAR_EPTYPE_CONTROL)
        wait_control_window(device);
    hcchar = dwc2_readl(device, DWC2_HCCHAR(DWC2_CHANNEL));
    dwc2_writel(device, DWC2_HCCHAR(DWC2_CHANNEL),
        hcchar | DWC2_HCCHAR_CHENA);
    return TRUE;
}

static BOOL arm_setup(struct DWC2Unit *unit)
{
    ULONG count;

    /* DWC2 retains non-periodic request-queue state across a prior channel
     * owner. A control SETUP armed against that stale state can sit forever
     * with CHENA set and HCINT clear (observed on QEMU). */
    dwc2_writel(unit->device, DWC2_GRSTCTL, DWC2_GRSTCTL_TXFFLSH);
    for (count = 0; count < 100; count++)
        if (!(dwc2_readl(unit->device, DWC2_GRSTCTL) & DWC2_GRSTCTL_TXFFLSH))
            break;
    if (count == 100)
    {
        bug("[DWC2/Emu68:XFER] setup: FIFO flush timed out GRSTCTL=%08lx\n",
            dwc2_readl(unit->device, DWC2_GRSTCTL));
        return FALSE;
    }

    CopyMem(&unit->active_request->iouh_SetupData, unit->dma_buffer, 8);
    unit->transfer_stage = STAGE_SETUP;
    return arm(unit, FALSE, DWC2_HCCHAR_EPTYPE_CONTROL,
        DWC2_HCTSIZ_PID_SETUP, 8);
}

static BOOL arm_control_data(struct DWC2Unit *unit)
{
    struct IOUsbHWReq *ioreq = unit->active_request;
    BOOL input = (ioreq->iouh_SetupData.bmRequestType & URTF_IN) != 0;

    if (!input && ioreq->iouh_Length != 0)
        CopyMem(ioreq->iouh_Data, unit->dma_buffer, ioreq->iouh_Length);
    unit->transfer_stage = STAGE_DATA;
    return arm(unit, input, DWC2_HCCHAR_EPTYPE_CONTROL,
        DWC2_HCTSIZ_PID_DATA1, ioreq->iouh_Length);
}

static BOOL arm_status(struct DWC2Unit *unit)
{
    struct IOUsbHWReq *ioreq = unit->active_request;
    BOOL input = (ioreq->iouh_SetupData.bmRequestType & URTF_IN) == 0;

    unit->transfer_stage = STAGE_STATUS;
    return arm(unit, input, DWC2_HCCHAR_EPTYPE_CONTROL,
        DWC2_HCTSIZ_PID_DATA1, 0);
}

static BOOL arm_interrupt(struct DWC2Unit *unit)
{
    struct IOUsbHWReq *ioreq = unit->active_request;
    ULONG toggle = (unit->data_toggle[ioreq->iouh_DevAddr & 0x7f] >>
        ioreq->iouh_Endpoint) & 1;

    unit->transfer_stage = STAGE_INT_IN;
    /* QEMU enforces periodic endpoint type and frame parity. arm() selects
     * the next microframe through HCCHAR.ODDFRM; the task still applies the
     * requested polling interval after NAK without enabling SOF interrupts. */
    return arm(unit, TRUE, DWC2_HCCHAR_EPTYPE_INTERRUPT,
        toggle ? DWC2_HCTSIZ_PID_DATA1 : DWC2_HCTSIZ_PID_DATA0,
        ioreq->iouh_Length);
}

BOOL dwc2_transfer_submit(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq)
{
    BOOL armed;

    if (unit->active_request != NULL)
    {
        AddTail(&unit->transfer_queue, &ioreq->iouh_Req.io_Message.mn_Node);
        return TRUE;
    }
    if (ioreq->iouh_Length > unit->dma_length)
        return FALSE;

    unit->active_request = ioreq;
    if (unit->transfer_log_count < 32)
    {
        unit->transfer_log_count++;
        bug("[DWC2/Emu68:XFER] submit #%u cmd=%u addr=%u ep=%u len=%lu "
            "interval=%u\n", unit->transfer_log_count,
            ioreq->iouh_Req.io_Command, ioreq->iouh_DevAddr,
            ioreq->iouh_Endpoint, ioreq->iouh_Length, ioreq->iouh_Interval);
    }
    unit->retry_count = 0;
    ioreq->iouh_Actual = 0;
    if (ioreq->iouh_Req.io_Command == UHCMD_CONTROLXFER)
        armed = arm_setup(unit);
    else if (ioreq->iouh_Req.io_Command == UHCMD_INTXFER &&
        ioreq->iouh_Dir == UHDIR_IN)
        armed = arm_interrupt(unit);
    else
        armed = FALSE;
    if (!armed)
        unit->active_request = NULL;
    return armed;
}

void dwc2_transfer_service(struct DWC2Unit *unit)
{
    struct Node *node = unit->periodic_queue.lh_Head;

    while (node->ln_Succ != NULL)
    {
        struct Node *next = node->ln_Succ;
        struct IOUsbHWReq *ioreq = (struct IOUsbHWReq *)node;

        if (ioreq->iouh_DriverPrivate2 != NULL)
        {
            Remove(node);
            ioreq->iouh_DriverPrivate1 = NULL;
            ioreq->iouh_Req.io_Error = IOERR_ABORTED;
            ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
            ReplyMsg(&ioreq->iouh_Req.io_Message);
        }
        node = next;
    }
    update_sof_irq(unit);
}

void dwc2_transfer_sof(struct DWC2Unit *unit)
{
    struct Node *node;
    ULONG now;

    dwc2_transfer_service(unit);
    if (unit->active_request != NULL)
        return;

    now = current_frame(unit);
    if (unit->sof_log_count < 8)
    {
        unit->sof_log_count++;
        bug("[DWC2/Emu68:SCHED] SOF #%u HFNUM=%08lx frame=%lu active=%p\n",
            unit->sof_log_count, dwc2_readl(unit->device, DWC2_HFNUM), now,
            unit->active_request);
    }
    node = unit->periodic_queue.lh_Head;
    while (node->ln_Succ != NULL)
    {
        struct IOUsbHWReq *ioreq = (struct IOUsbHWReq *)node;
        ULONG due = (ULONG)(IPTR)ioreq->iouh_DriverPrivate1 &
            DWC2_FRAME_MASK;

        node = node->ln_Succ;
        if (!frame_due(now, due))
            continue;
        Remove(&ioreq->iouh_Req.io_Message.mn_Node);
        ioreq->iouh_DriverPrivate1 = NULL;
        update_sof_irq(unit);
        if (!dwc2_transfer_submit(unit, ioreq))
        {
            ioreq->iouh_Req.io_Error = UHIOERR_HOSTERROR;
            ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
            ReplyMsg(&ioreq->iouh_Req.io_Message);
        }
        return;
    }
}

static void retry(struct DWC2Unit *unit)
{
    if (++unit->retry_count > 100)
    {
        finish(unit, UHIOERR_TIMEOUT);
        return;
    }
    dwc2_delay_us(unit, unit->transfer_stage == STAGE_INT_IN ? 10000 : 1000);
    if (unit->transfer_stage == STAGE_SETUP)
        arm_setup(unit);
    else if (unit->transfer_stage == STAGE_DATA)
        arm_control_data(unit);
    else if (unit->transfer_stage == STAGE_STATUS)
        arm_status(unit);
    else
        arm_interrupt(unit);
}

void dwc2_transfer_irq(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;
    struct IOUsbHWReq *ioreq = unit->active_request;
    ULONG status = unit->channel_pending;
    ULONG remaining;
    ULONG moved;

    unit->channel_pending = 0;
    if (status == 0)
        status = dwc2_readl(device, DWC2_HCINT(DWC2_CHANNEL));
    dwc2_writel(device, DWC2_HCINT(DWC2_CHANNEL), status);
    if (ioreq == NULL)
        return;
    if (unit->transfer_log_count < 32)
    {
        unit->transfer_log_count++;
        bug("[DWC2/Emu68:XFER] irq #%u stage=%u HCINT=%08lx HCTSIZ=%08lx "
            "HCCHAR=%08lx HFNUM=%08lx\n",
            unit->transfer_log_count, unit->transfer_stage, status,
            dwc2_readl(device, DWC2_HCTSIZ(DWC2_CHANNEL)),
            dwc2_readl(device, DWC2_HCCHAR(DWC2_CHANNEL)),
            dwc2_readl(device, DWC2_HFNUM));
    }
    if (status & (DWC2_HCINT_STALL | DWC2_HCINT_AHBERR |
                  DWC2_HCINT_XACTERR | DWC2_HCINT_BBLERR |
                  DWC2_HCINT_DATATGLERR))
    {
        bug("[DWC2/Emu68:XFER] error stage=%lu HCINT=%08lx\n",
            (ULONG)unit->transfer_stage, status);
        finish(unit, (status & DWC2_HCINT_STALL) ? UHIOERR_STALL : UHIOERR_HOSTERROR);
        return;
    }
    if ((status & DWC2_HCINT_NAK) && !(status & DWC2_HCINT_XFERCOMP))
    {
        if (unit->transfer_stage == STAGE_INT_IN)
        {
            ULONG interval = ioreq->iouh_Interval;

            if (interval == 0)
                interval = 1;
            if (interval > DWC2_FRAME_MASK)
                interval = DWC2_FRAME_MASK;
            unit->active_request = NULL;
            unit->transfer_stage = 0;
            dwc2_writel(device, DWC2_HAINTMSK, 0);
            dwc2_writel(device, DWC2_GINTMSK,
                dwc2_readl(device, DWC2_GINTMSK) &
                    ~DWC2_GINTSTS_HCHINT);
            ioreq->iouh_DriverPrivate1 = (APTR)(IPTR)
                ((current_frame(unit) + interval) & DWC2_FRAME_MASK);
            if (unit->periodic_log_count < 8)
            {
                unit->periodic_log_count++;
                bug("[DWC2/Emu68:SCHED] NAK #%u HFNUM=%08lx frame=%lu "
                    "interval=%lu due=%lu\n", unit->periodic_log_count,
                    dwc2_readl(device, DWC2_HFNUM), current_frame(unit),
                    interval, (ULONG)(IPTR)ioreq->iouh_DriverPrivate1);
            }
            AddTail(&unit->periodic_queue,
                &ioreq->iouh_Req.io_Message.mn_Node);
            update_sof_irq(unit);
            start_next(unit);
            return;
        }
        retry(unit);
        return;
    }
    if ((status & DWC2_HCINT_CHHLTD) &&
        !(status & DWC2_HCINT_XFERCOMP))
    {
        if (!reset_halted_channel(unit))
        {
            finish(unit, UHIOERR_HOSTERROR);
            return;
        }
        retry(unit);
        return;
    }
    if (!(status & DWC2_HCINT_XFERCOMP))
        return;

    unit->retry_count = 0;
    remaining = dwc2_readl(device, DWC2_HCTSIZ(DWC2_CHANNEL)) & 0x7ffff;
    moved = unit->active_length >= remaining ? unit->active_length - remaining : 0;

    if (unit->transfer_stage == STAGE_SETUP)
    {
        if (ioreq->iouh_Length != 0)
        {
            if (!arm_control_data(unit))
                finish(unit, UHIOERR_HOSTERROR);
        }
        else if (!arm_status(unit))
            finish(unit, UHIOERR_HOSTERROR);
    }
    else if (unit->transfer_stage == STAGE_DATA)
    {
        if (ioreq->iouh_SetupData.bmRequestType & URTF_IN)
        {
            CacheClearE(unit->dma_buffer, moved, CACRF_InvalidateD);
            CopyMem(unit->dma_buffer, ioreq->iouh_Data, moved);
        }
        ioreq->iouh_Actual = moved;
        if (!arm_status(unit))
            finish(unit, UHIOERR_HOSTERROR);
    }
    else if (unit->transfer_stage == STAGE_STATUS)
        finish(unit, 0);
    else
    {
        ULONG address = ioreq->iouh_DevAddr & 0x7f;

        CacheClearE(unit->dma_buffer, moved, CACRF_InvalidateD);
        CopyMem(unit->dma_buffer, ioreq->iouh_Data, moved);
        ioreq->iouh_Actual = moved;
        if (unit->interrupt_log_count[address] < 16)
        {
            unit->interrupt_log_count[address]++;
            bug("[DWC2/Emu68] interrupt data #%u addr=%lu ep=%u bytes=%lu "
                "interval=%u data=%02x %02x %02x %02x %02x %02x\n",
                unit->interrupt_log_count[address],
                address, ioreq->iouh_Endpoint, moved, ioreq->iouh_Interval,
                moved > 0 ? unit->dma_buffer[0] : 0,
                moved > 1 ? unit->dma_buffer[1] : 0,
                moved > 2 ? unit->dma_buffer[2] : 0,
                moved > 3 ? unit->dma_buffer[3] : 0,
                moved > 4 ? unit->dma_buffer[4] : 0,
                moved > 5 ? unit->dma_buffer[5] : 0);
        }
        unit->data_toggle[ioreq->iouh_DevAddr & 0x7f] ^=
            1UL << ioreq->iouh_Endpoint;
        finish(unit, 0);
    }
}
