/*
 * Copyright (C) 2026, The Bellatrix Project.
 *
 * DWC2 transfer engine. The unit task is the sole owner of every channel;
 * the hardware IRQ only gates the controller, records which channels raised
 * an interrupt, and wakes that task.
 *
 * A channel is the unit of concurrency. Each one carries its own request,
 * stage, retry budget and bounce buffer (struct DWC2Channel), so nothing here
 * reads ambient "current transfer" state -- the channel is always an
 * argument. That is the property the inherited engine lacks, and the one that
 * makes "which transfer is this interrupt about" answerable without guessing.
 *
 * Endianness: every register access goes through dwc2_readl/dwc2_writel,
 * which state that DWC2 registers are little-endian. Nothing here dereferences
 * a register directly, and nothing assumes the host's byte order.
 */

#include <aros/debug.h>

#include <asm/cpu.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "dwc2emu68_intern.h"
#include "dwc2emu68_regs.h"

#define STAGE_SETUP  1
#define STAGE_DATA   2
#define STAGE_STATUS 3
#define STAGE_INT_IN 4
#define STAGE_BULK   5
#define STAGE_ISO    6

#define DWC2_FRAME_MASK 0x07ffUL
#define DWC2_CONTROL_GATE_LOOPS 8000000UL
#define DWC2_RETRY_LIMIT 100
/*
 * Three attempts at a transaction before its error is believed -- the number
 * USB itself specifies. It is deliberately not the NAK budget above: a NAK is
 * the device asking to be asked again and costs nothing to honour, while a
 * transaction error is a bus event that must not be retried forever.
 */
#define DWC2_XACT_ERROR_LIMIT 3
/*
 * How many arm/completion lines the serial line is allowed to carry. It used
 * to be 32, which covered the root hub and the hub behind it and ran out
 * exactly at the first device -- so three rounds of hardware testing produced
 * a failure whose transfers were never shown. A diagnostic budget that stops
 * before the interesting part is worse than none: it looks like data.
 */
#define DWC2_TRANSFER_LOG_LIMIT 40

/* The account a line is drawn from: endpoint 0 and the data endpoints keep
 * separate budgets, so enumeration cannot spend what the interrupt endpoint
 * needs. */
#define DWC2_LOG_BUDGET(u, r)                                            \
    ((r)->iouh_Endpoint == 0                                             \
        ? &(u)->transfer_log[(r)->iouh_DevAddr & 0x7f]                   \
        : &(u)->transfer_log_ep[(r)->iouh_DevAddr & 0x7f])

static void start_next(struct DWC2Unit *unit);
static BOOL split_needed(struct DWC2Unit *unit,
    const struct IOUsbHWReq *ioreq);
static BOOL submit_on(struct DWC2Unit *unit, struct DWC2Channel *chan,
    struct IOUsbHWReq *ioreq);
static BOOL arm_setup(struct DWC2Unit *unit, struct DWC2Channel *chan);
static BOOL arm_control_data(struct DWC2Unit *unit, struct DWC2Channel *chan);
static BOOL arm_status(struct DWC2Unit *unit, struct DWC2Channel *chan);
static BOOL arm_bulk(struct DWC2Unit *unit, struct DWC2Channel *chan);
static BOOL arm_iso(struct DWC2Unit *unit, struct DWC2Channel *chan);
static BOOL arm_interrupt(struct DWC2Unit *unit, struct DWC2Channel *chan);

/*
 * Channel bookkeeping.
 *
 * host_channels is what the core reported at start-up, clamped to the array.
 * Allocation is first-fit and deliberately dumb: the DWC2 arbitrates between
 * armed channels itself, so there is nothing to gain from choosing cleverly,
 * and a stable rule is easier to follow in a trace.
 */
static struct DWC2Channel *channel_alloc(struct DWC2Unit *unit)
{
    UBYTE i;

    for (i = 0; i < unit->host_channels; i++)
        if (unit->channel[i].request == NULL)
            return &unit->channel[i];
    return NULL;
}

BOOL dwc2_transfer_idle(const struct DWC2Unit *unit)
{
    UBYTE i;

    for (i = 0; i < unit->host_channels; i++)
        if (unit->channel[i].request != NULL)
            return FALSE;
    return TRUE;
}

/* Does any channel hold a request Poseidon has asked us to abort? */
BOOL dwc2_transfer_pending_abort(const struct DWC2Unit *unit)
{
    UBYTE i;

    for (i = 0; i < unit->host_channels; i++)
    {
        const struct IOUsbHWReq *req = unit->channel[i].request;

        if (req != NULL && req->iouh_DriverPrivate2 != NULL)
            return TRUE;
    }
    return FALSE;
}

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

    (void)enabled;
    /* Keep SOF running while hardware bring-up is being validated.  The
     * established BCM2708 driver operates this way, and correctness on the
     * OT2.80a core takes precedence over deadline-driven IRQ masking here. */
    mask |= DWC2_GINTSTS_SOF;
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

/*
 * Does this transfer really need a split transaction?
 *
 * Poseidon sets UHFF_SPLITTRANS from a rule about descriptors -- a USB 1.1
 * device behind a USB 2.0 hub -- and it is right about that rule. What it
 * cannot know is whether the hub it names has a transaction translator this
 * controller can address, and there are two cases where it does not:
 *
 *   - the root port did not come up at high speed. Then the whole bus below
 *     it runs at that speed, spoken directly, and no translator exists
 *     anywhere on it.
 *   - the named hub is our own virtual root hub. It is a description of a
 *     port, not a chip; there is no translator inside it to ask.
 *
 * Programming HCSPLT anyway is not a slow transfer, it is a dead one: the
 * core issues a start-split addressed to nothing, the channel stays enabled
 * with CHENA set, and no interrupt ever arrives -- which is exactly what a
 * Raspberry Pi 3 does at the first SETUP of enumeration.
 */
static BOOL split_needed(struct DWC2Unit *unit,
    const struct IOUsbHWReq *ioreq)
{
    ULONG hprt;

    if (!(ioreq->iouh_Flags & UHFF_SPLITTRANS))
        return FALSE;
    if (ioreq->iouh_SplitHubAddr == unit->hub_address)
        return FALSE;
    hprt = dwc2_readl(unit->device, DWC2_HPRT);
    return (hprt & DWC2_HPRT_SPD_MASK) == DWC2_HPRT_SPD_HIGH;
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

static BOOL reset_halted_channel(struct DWC2Unit *unit,
    struct DWC2Channel *chan)
{
    struct DWC2Device *device = unit->device;
    ULONG hcchar = dwc2_readl(device, DWC2_HCCHAR(chan->index));
    ULONG count;

    /* A bare CHHLTD leaves the BCM2837 channel state machine unable to run a
     * later transaction. Drive one real disable cycle while the endpoint
     * context is still present, then scrub the channel before reprogramming. */
    dwc2_writel(device, DWC2_HCCHAR(chan->index),
        hcchar | DWC2_HCCHAR_CHENA | DWC2_HCCHAR_CHDIS);
    for (count = 0; count < 200000; count++)
        if (!(dwc2_readl(device, DWC2_HCCHAR(chan->index)) &
            DWC2_HCCHAR_CHENA))
            break;
    if (count == 200000)
    {
        bug("[DWC2/Emu68:XFER] chan=%u recovery timed out HCCHAR=%08lx\n",
            chan->index, dwc2_readl(device, DWC2_HCCHAR(chan->index)));
        return FALSE;
    }
    dwc2_writel(device, DWC2_HCINT(chan->index), DWC2_HCINT_ALL);
    dwc2_writel(device, DWC2_HCSPLT(chan->index), 0);
    dwc2_writel(device, DWC2_HCCHAR(chan->index), 0);
    return TRUE;
}

/*
 * Stop a channel raising interrupts and release it.
 *
 * HAINTMSK is a per-channel bitmap, so releasing one channel must clear only
 * its own bit; the single-channel engine wrote HAINTMSK = 0 here and would
 * have silenced every other channel in flight. GINTSTS.HCHINT is only
 * withdrawn once no channel is left armed.
 */
static void channel_release(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    struct DWC2Device *device = unit->device;

    /* Order matters: the interrupt half reads chan->request while a paced
     * complete-split is outstanding, so withdraw it from that set first. */
    unit->split_pacing &= ~(1UL << chan->index);
    /*
     * A split sequence that ends any way other than by completing leaves the
     * channel's split engine mid-sequence, and every later start-split on
     * that channel is descheduled in its arming microframe with a bare
     * CHHLTD. STALL is the ending that makes this bite in practice, because
     * STALL is not a driver failure at all -- it is the device declining a
     * request, which every device does for the string indices it has not
     * implemented, several times per enumeration.
     */
    if (chan->split != DWC2_SPLIT_NONE)
        reset_halted_channel(unit, chan);
    chan->request = NULL;
    chan->stage = 0;
    chan->retries = 0;
    chan->xact_errors = 0;
    chan->armed_length = 0;
    chan->watchdog_ticks = 0;
    chan->split = DWC2_SPLIT_NONE;
    chan->split_retries = 0;
    chan->split_delay = 0;
    chan->pending = 0;
    dwc2_writel(device, DWC2_HCINTMSK(chan->index), 0);
    dwc2_writel(device, DWC2_HAINTMSK,
        dwc2_readl(device, DWC2_HAINTMSK) & ~(1UL << chan->index));
    if (dwc2_transfer_idle(unit))
        dwc2_writel(device, DWC2_GINTMSK,
            dwc2_readl(device, DWC2_GINTMSK) & ~DWC2_GINTSTS_HCHINT);
}

static void finish(struct DWC2Unit *unit, struct DWC2Channel *chan,
    BYTE error)
{
    struct IOUsbHWReq *ioreq = chan->request;

    channel_release(unit, chan);
    ioreq->iouh_DriverPrivate1 = NULL;
    ioreq->iouh_Req.io_Error = error;
    ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
    if (!(ioreq->iouh_Req.io_Flags & IOF_QUICK))
        ReplyMsg(&ioreq->iouh_Req.io_Message);

    start_next(unit);
}

/*
 * Fill every free channel from the waiting queue.
 *
 * The single-channel engine returned after arming one request, because there
 * was only ever one place to put it. With eight channels the queue should be
 * drained until either it is empty or the hardware is full, otherwise a burst
 * of submissions is serialised for no reason.
 */
/*
 * Is another transaction already in flight to this exact endpoint?
 *
 * An endpoint has one data toggle and one hardware state machine. Two
 * transactions to it at once race over both, and the toggle they share ends
 * up describing neither. This could not happen while there was one channel;
 * it can now, so it has to be refused.
 *
 * Device address, endpoint number AND direction, because endpoint 1 IN and
 * endpoint 1 OUT are two endpoints.
 */
static BOOL endpoint_in_flight(struct DWC2Unit *unit,
    const struct IOUsbHWReq *candidate)
{
    UBYTE i;

    for (i = 0; i < unit->host_channels; i++)
    {
        const struct IOUsbHWReq *active = unit->channel[i].request;

        if (active != NULL && active != candidate &&
            active->iouh_DevAddr == candidate->iouh_DevAddr &&
            active->iouh_Endpoint == candidate->iouh_Endpoint &&
            active->iouh_Dir == candidate->iouh_Dir)
            return TRUE;
    }
    return FALSE;
}

/*
 * Is another split already running on this hub's transaction translator?
 *
 * At most one split transaction of any type per hub. This is not a
 * throughput choice: ../usb2otg records that overlapping split streams take
 * the channel down and that cross-device damage on a shared hub has been
 * observed. On a Raspberry Pi 3 every port is behind one LAN9514, so a
 * keyboard and a mouse are precisely the pair this protects.
 *
 * Matched on the hub address alone and not hub+port, because the hubs in
 * play here are single-TT: all their ports share one translator.
 */
static BOOL tt_in_flight(struct DWC2Unit *unit,
    const struct IOUsbHWReq *candidate)
{
    UBYTE i;

    /* Asked about the real thing, not the flag: a request whose split was
     * declined by split_needed() runs directly and holds no translator. */
    if (!split_needed(unit, candidate))
        return FALSE;
    for (i = 0; i < unit->host_channels; i++)
    {
        const struct IOUsbHWReq *active = unit->channel[i].request;

        if (active != NULL && active != candidate &&
            split_needed(unit, active) &&
            active->iouh_SplitHubAddr == candidate->iouh_SplitHubAddr)
            return TRUE;
    }
    return FALSE;
}

/*
 * May this request be armed at this instant?
 *
 * Three independent reasons to wait, all of them consequences of running
 * more than one channel:
 *
 *   - a control transfer opens with a global non-periodic FIFO flush (see
 *     arm_setup), which would discard whatever another channel has queued
 *     there, so it waits for the hardware to go quiet;
 *   - the endpoint may already be busy;
 *   - the hub's translator may already be busy.
 */
static BOOL can_arm_now(struct DWC2Unit *unit, const struct IOUsbHWReq *ioreq)
{
    if (ioreq->iouh_Req.io_Command == UHCMD_CONTROLXFER &&
        !dwc2_transfer_idle(unit))
        return FALSE;
    if (endpoint_in_flight(unit, ioreq))
        return FALSE;
    if (tt_in_flight(unit, ioreq))
        return FALSE;
    return TRUE;
}

static void start_next(struct DWC2Unit *unit)
{
    struct DWC2Channel *chan;

    while ((chan = channel_alloc(unit)) != NULL)
    {
        struct IOUsbHWReq *next =
            (struct IOUsbHWReq *)unit->transfer_queue.lh_Head;

        /* Peek before taking: a request that cannot be armed yet has to stay
         * where it is. Taking it and putting it back would spin here. */
        if (next->iouh_Req.io_Message.mn_Node.ln_Succ == NULL)
            return;
        if (next->iouh_DriverPrivate2 == NULL && !can_arm_now(unit, next))
            return;
        Remove(&next->iouh_Req.io_Message.mn_Node);
        if (next->iouh_DriverPrivate2 != NULL)
        {
            next->iouh_Req.io_Error = IOERR_ABORTED;
            next->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
            ReplyMsg(&next->iouh_Req.io_Message);
            continue;
        }
        if (!submit_on(unit, chan, next))
        {
            next->iouh_Req.io_Error = UHIOERR_HOSTERROR;
            next->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
            ReplyMsg(&next->iouh_Req.io_Message);
        }
    }
}

static void abort_channel(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    struct DWC2Device *device = unit->device;
    ULONG hcchar;
    ULONG count;

    if (chan->request == NULL)
        return;
    hcchar = dwc2_readl(device, DWC2_HCCHAR(chan->index));
    if (hcchar & DWC2_HCCHAR_CHENA)
    {
        dwc2_writel(device, DWC2_HCCHAR(chan->index),
            hcchar | DWC2_HCCHAR_CHDIS | DWC2_HCCHAR_CHENA);
        for (count = 0; count < 100; count++)
            if (!(dwc2_readl(device, DWC2_HCCHAR(chan->index)) &
                DWC2_HCCHAR_CHENA))
                break;
    }
    dwc2_writel(device, DWC2_HCINT(chan->index), DWC2_HCINT_ALL);
    finish(unit, chan, IOERR_ABORTED);
}

void dwc2_transfer_abort_all(struct DWC2Unit *unit)
{
    UBYTE i;

    for (i = 0; i < unit->host_channels; i++)
    {
        struct IOUsbHWReq *req = unit->channel[i].request;

        if (req != NULL && req->iouh_DriverPrivate2 != NULL)
            abort_channel(unit, &unit->channel[i]);
    }
}

static BOOL arm(struct DWC2Unit *unit, struct DWC2Channel *chan, BOOL input,
    ULONG endpoint_type, ULONG pid, ULONG length)
{
    struct IOUsbHWReq *ioreq = chan->request;
    struct DWC2Device *device = unit->device;
    ULONG mps = ioreq->iouh_MaxPktSize;
    BOOL split = split_needed(unit, ioreq);
    ULONG packets;
    ULONG hcchar;
    ULONG mask;

    if (mps == 0 || length > chan->buffer_size)
        return FALSE;

    /*
     * A start-split hands the hub one full-speed frame's worth of work at
     * most, so a split transaction is armed in pieces of at most 188 bytes.
     * Asking for more makes the translator return data the core was not told
     * to expect. Bulk and the control data stage both advance through
     * iouh_Actual, so a clamped arming is a chunk and not a truncation.
     * Anything without a continuation must not be clamped.
     */
    if (split && length > DWC2_SPLIT_MAX_PAYLOAD)
    {
        /*
         * Truncate to a whole number of packets, never mid-packet: a final
         * short packet is how a device says "that is all there was", so a
         * chunk boundary that happened to fall inside a packet would end the
         * transfer early and silently. 188 is not a multiple of any legal
         * MaxPktSize, so this always rounds down -- to 128 for a 64-byte
         * full-speed bulk endpoint.
         */
        length = (DWC2_SPLIT_MAX_PAYLOAD / mps) * mps;
        if (length == 0)
            length = mps;
    }
    packets = length == 0 ? 1 : (length + mps - 1) / mps;
    chan->armed_length = length;
    chan->watchdog_ticks = 0;
    chan->split = split ? DWC2_SPLIT_START : DWC2_SPLIT_NONE;
    chan->split_retries = 0;

    if (input && length != 0)
        CacheClearE(chan->buffer, length, CACRF_InvalidateD);
    else if (length != 0)
        CacheClearE(chan->buffer, length, CACRF_ClearD);
    dsb();

    hcchar = DWC2_HCCHAR_DEVADDR(ioreq->iouh_DevAddr) |
        DWC2_HCCHAR_EPNUM(ioreq->iouh_Endpoint) |
        DWC2_HCCHAR_MPS(mps) | DWC2_HCCHAR_MULTICNT_ONE | endpoint_type;
    if (input)
        hcchar |= DWC2_HCCHAR_EPDIR_IN;
    if (ioreq->iouh_Flags & UHFF_LOWSPEED)
        hcchar |= DWC2_HCCHAR_LSPDDEV;
    if (endpoint_type == DWC2_HCCHAR_EPTYPE_INTERRUPT &&
        !(dwc2_readl(device, DWC2_HFNUM) & 1))
        hcchar |= DWC2_HCCHAR_ODDFRM;

    /* Program the endpoint characteristics while the channel is disabled.
     * The core latches this context before HCTSIZ/HCDMA; QEMU, like the
     * hardware, does not start a channel reliably when HCCHAR is written only
     * once with CHENA after the other registers. */
    dwc2_writel(device, DWC2_HCCHAR(chan->index), hcchar);
    /*
     * HCSPLT names the translator, not the device: which hub, which of its
     * ports. Poseidon fills iouh_SplitHubAddr/Port in for exactly this, and
     * leaves UHFF_SPLITTRANS clear when the device is high-speed and can be
     * addressed directly.
     */
    dwc2_writel(device, DWC2_HCSPLT(chan->index), split ?
        (DWC2_HCSPLT_SPLTENA | DWC2_HCSPLT_XACTPOS_ALL |
         DWC2_HCSPLT_HUBADDR(ioreq->iouh_SplitHubAddr) |
         DWC2_HCSPLT_PRTADDR(ioreq->iouh_SplitHubPort)) : 0);
    dwc2_writel(device, DWC2_HCINT(chan->index), DWC2_HCINT_ALL);
    /*
     * NYET is unmasked for splits only.
     *
     * On a split it is the pacing signal -- the translator saying "not
     * finished", which the complete-split sequencer needs to see. On anything
     * else the buffer-DMA core drives NAK, NYET and the PING protocol by
     * itself, and taking the interrupt means halting the channel in the
     * middle of a burst the core was managing. ../usb2otg records that as
     * fighting the core and opening wedge windows, and this engine has an
     * unexplained set of watchdog recoveries on control data stages that
     * completed without raising a channel interrupt -- which is the shape a
     * wedge window leaves.
     *
     * ACK is unmasked for splits for the same reason: it is how a start-split
     * says the translator took the transaction.
     */
    mask = DWC2_HCINT_XFERCOMP | DWC2_HCINT_CHHLTD |
        DWC2_HCINT_AHBERR | DWC2_HCINT_STALL | DWC2_HCINT_NAK |
        DWC2_HCINT_XACTERR | DWC2_HCINT_BBLERR | DWC2_HCINT_DATATGLERR;
    if (split)
        mask |= DWC2_HCINT_NYET | DWC2_HCINT_ACK;
    dwc2_writel(device, DWC2_HCINTMSK(chan->index), mask);
    dwc2_writel(device, DWC2_HCTSIZ(chan->index),
        DWC2_HCTSIZ_XFERSIZE(length) | DWC2_HCTSIZ_PKTCNT(packets) | pid);
    dwc2_writel(device, DWC2_HCDMA(chan->index), dma_address(chan->buffer));
    /* Read-modify-write: another channel may already be armed and its bit in
     * HAINTMSK must survive. */
    dwc2_writel(device, DWC2_HAINTMSK,
        dwc2_readl(device, DWC2_HAINTMSK) | (1UL << chan->index));
    dwc2_writel(device, DWC2_GINTMSK,
        dwc2_readl(device, DWC2_GINTMSK) | DWC2_GINTSTS_HCHINT);
    if (endpoint_type == DWC2_HCCHAR_EPTYPE_CONTROL)
        wait_control_window(device);
    if (*DWC2_LOG_BUDGET(unit, chan->request) < DWC2_TRANSFER_LOG_LIMIT)
        bug("[DWC2/Emu68:XFER] arm chan=%u stage=%u CHAR=%08lx TSIZ=%08lx "
            "SPLT=%08lx NPTX=%08lx HFNUM=%08lx\n", chan->index, chan->stage,
            dwc2_readl(device, DWC2_HCCHAR(chan->index)),
            dwc2_readl(device, DWC2_HCTSIZ(chan->index)),
            dwc2_readl(device, DWC2_HCSPLT(chan->index)),
            dwc2_readl(device, DWC2_GNPTXSTS),
            dwc2_readl(device, DWC2_HFNUM));
    hcchar = dwc2_readl(device, DWC2_HCCHAR(chan->index));
    dwc2_writel(device, DWC2_HCCHAR(chan->index),
        hcchar | DWC2_HCCHAR_CHENA);
    return TRUE;
}

static BOOL arm_setup(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    ULONG count;

    /* DWC2 retains non-periodic request-queue state across a prior channel
     * owner. A control SETUP armed against that stale state can sit forever
     * with CHENA set and HCINT clear (observed on QEMU).
     *
     * The flush is global, so it may only be done while this is the only
     * non-periodic channel armed -- otherwise it would discard another
     * channel's queued request. dwc2_transfer_idle() is checked by the caller
     * for exactly that reason. */
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

    CopyMem(&chan->request->iouh_SetupData, chan->buffer, 8);
    chan->stage = STAGE_SETUP;
    return arm(unit, chan, FALSE, DWC2_HCCHAR_EPTYPE_CONTROL,
        DWC2_HCTSIZ_PID_SETUP, 8);
}

static BOOL arm_control_data(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    struct IOUsbHWReq *ioreq = chan->request;
    BOOL input = (ioreq->iouh_SetupData.bmRequestType & URTF_IN) != 0;
    ULONG remaining = ioreq->iouh_Length - ioreq->iouh_Actual;
    ULONG pid = DWC2_HCTSIZ_PID_DATA1;

    if (remaining > chan->buffer_size)
        remaining = chan->buffer_size;
    /*
     * This is not always the first chunk of the data stage. A split clamps
     * what one arming can carry, so a long read comes back in pieces, and a
     * continuation has to carry the toggle on rather than restart it. The
     * first chunk is DATA1 by definition; after that the core has left the
     * PID it expects next in HCTSIZ, which is the authority.
     *
     * The endpoint toggle map is deliberately not consulted. A control
     * transfer's toggle restarts at every SETUP and belongs to the transfer,
     * not to the endpoint, which is the opposite of how bulk works.
     */
    if (ioreq->iouh_Actual != 0)
        pid = dwc2_readl(unit->device, DWC2_HCTSIZ(chan->index)) &
            DWC2_HCTSIZ_PID_MASK;
    if (!input && remaining != 0)
        CopyMem((UBYTE *)ioreq->iouh_Data + ioreq->iouh_Actual,
            chan->buffer, remaining);
    chan->stage = STAGE_DATA;
    return arm(unit, chan, input, DWC2_HCCHAR_EPTYPE_CONTROL, pid, remaining);
}

static BOOL arm_status(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    struct IOUsbHWReq *ioreq = chan->request;
    BOOL input = (ioreq->iouh_SetupData.bmRequestType & URTF_IN) == 0;

    chan->stage = STAGE_STATUS;
    return arm(unit, chan, input, DWC2_HCCHAR_EPTYPE_CONTROL,
        DWC2_HCTSIZ_PID_DATA1, 0);
}

/*
 * Data toggle, per endpoint AND per direction.
 *
 * data_toggle[] is one ULONG per device address, and this used to index it by
 * endpoint number alone. USB endpoint 1 IN and endpoint 1 OUT are two
 * different endpoints with two independent toggles, so a device using both --
 * which every bulk device does -- would have had them share one bit and
 * desynchronise. Sixteen endpoints in the low half, the same sixteen IN in
 * the high half, which is exactly the 32 bits available.
 */
static ULONG toggle_bit(struct IOUsbHWReq *ioreq, BOOL input)
{
    return 1UL << ((ioreq->iouh_Endpoint & 15) + (input ? 16 : 0));
}

static ULONG toggle_pid(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq,
    BOOL input)
{
    return (unit->data_toggle[ioreq->iouh_DevAddr & 0x7f] &
        toggle_bit(ioreq, input)) ? DWC2_HCTSIZ_PID_DATA1
                                  : DWC2_HCTSIZ_PID_DATA0;
}

static void toggle_flip(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq,
    BOOL input)
{
    unit->data_toggle[ioreq->iouh_DevAddr & 0x7f] ^= toggle_bit(ioreq, input);
}

/*
 * One chunk of a bulk transfer.
 *
 * The bounce buffer is finite and a bulk request can be far larger -- a mass
 * storage read is routinely 64 KB -- so a request is armed a chunk at a time
 * and iouh_Actual carries the progress between them.
 */
static BOOL arm_bulk(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    struct IOUsbHWReq *ioreq = chan->request;
    BOOL input = (ioreq->iouh_Dir == UHDIR_IN);
    ULONG remaining = ioreq->iouh_Length - ioreq->iouh_Actual;
    ULONG length = remaining > chan->buffer_size ? chan->buffer_size
                                                 : remaining;

    if (!input && length != 0)
        CopyMem((UBYTE *)ioreq->iouh_Data + ioreq->iouh_Actual,
            chan->buffer, length);
    chan->stage = STAGE_BULK;
    return arm(unit, chan, input, DWC2_HCCHAR_EPTYPE_BULK,
        toggle_pid(unit, ioreq, input), length);
}

/*
 * One isochronous service interval.
 *
 * Isochronous is the transfer type with no handshake: the device neither
 * acknowledges nor refuses, and a transaction that misses its frame is lost
 * rather than retried. That is the contract -- an audio stream would rather
 * drop a millisecond than replay it late -- and it is why this path has no
 * retry budget and no NAK handling at all.
 *
 * The consequence for error reporting is that a short or missed transaction
 * is reported through iouh_Actual, not through io_Error. A caller that asked
 * for 192 bytes and got 180 was not failed; it lost twelve bytes of audio.
 */
static BOOL arm_iso(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    struct IOUsbHWReq *ioreq = chan->request;
    BOOL input = (ioreq->iouh_Dir == UHDIR_IN);
    ULONG remaining = ioreq->iouh_Length - ioreq->iouh_Actual;
    ULONG length = remaining > chan->buffer_size ? chan->buffer_size
                                                 : remaining;

    if (!input && length != 0)
        CopyMem((UBYTE *)ioreq->iouh_Data + ioreq->iouh_Actual,
            chan->buffer, length);
    chan->stage = STAGE_ISO;
    /* No toggle: isochronous endpoints use DATA0 for every transaction of a
     * single-transaction-per-microframe endpoint. */
    return arm(unit, chan, input, DWC2_HCCHAR_EPTYPE_ISOC,
        DWC2_HCTSIZ_PID_DATA0, length);
}

static BOOL arm_interrupt(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    struct IOUsbHWReq *ioreq = chan->request;

    chan->stage = STAGE_INT_IN;
    /* QEMU enforces periodic endpoint type and frame parity. arm() selects
     * the next microframe through HCCHAR.ODDFRM; the task still applies the
     * requested polling interval after NAK without enabling SOF interrupts. */
    return arm(unit, chan, TRUE, DWC2_HCCHAR_EPTYPE_INTERRUPT,
        toggle_pid(unit, ioreq, TRUE), ioreq->iouh_Length);
}

/*
 * Put one request on one channel.
 *
 * The caller has already decided the channel is free. A control transfer
 * additionally needs the non-periodic FIFO to itself for its opening flush
 * (see arm_setup), so it waits for the hardware to go quiet rather than
 * disturbing another channel's queued request.
 */
static BOOL submit_on(struct DWC2Unit *unit, struct DWC2Channel *chan,
    struct IOUsbHWReq *ioreq)
{
    BOOL armed;

    /* Bulk is armed a chunk at a time and may exceed the bounce buffer; the
     * others are armed once and may not. */
    if (ioreq->iouh_Req.io_Command != UHCMD_BULKXFER &&
        ioreq->iouh_Length > chan->buffer_size)
    {
        /*
         * Say so. A refusal here is the one way a request can reach the
         * hardware layer and leave no trace at all, and a transfer that was
         * never printed is indistinguishable from one that was never made.
         */
        if (*DWC2_LOG_BUDGET(unit, ioreq) < DWC2_TRANSFER_LOG_LIMIT)
        {
            (*DWC2_LOG_BUDGET(unit, ioreq))++;
            bug("[DWC2/Emu68:XFER] refused cmd=%u addr=%u ep=%u len=%lu "
                "exceeds the %lu byte channel buffer\n",
                ioreq->iouh_Req.io_Command, ioreq->iouh_DevAddr,
                ioreq->iouh_Endpoint, ioreq->iouh_Length,
                chan->buffer_size);
        }
        return FALSE;
    }
    chan->request = ioreq;
    chan->retries = 0;
    chan->xact_errors = 0;
    if (*DWC2_LOG_BUDGET(unit, ioreq) < DWC2_TRANSFER_LOG_LIMIT)
    {
        (*DWC2_LOG_BUDGET(unit, ioreq))++;
        unit->transfer_log_count++;
        bug("[DWC2/Emu68:XFER] submit #%u chan=%u cmd=%u addr=%u ep=%u "
            "len=%lu interval=%u\n", unit->transfer_log_count, chan->index,
            ioreq->iouh_Req.io_Command, ioreq->iouh_DevAddr,
            ioreq->iouh_Endpoint, ioreq->iouh_Length, ioreq->iouh_Interval);
    }
    ioreq->iouh_Actual = 0;
    if (ioreq->iouh_Req.io_Command == UHCMD_CONTROLXFER)
        armed = arm_setup(unit, chan);
    else if (ioreq->iouh_Req.io_Command == UHCMD_INTXFER &&
        ioreq->iouh_Dir == UHDIR_IN)
        armed = arm_interrupt(unit, chan);
    else if (ioreq->iouh_Req.io_Command == UHCMD_BULKXFER)
        armed = arm_bulk(unit, chan);
    else if (ioreq->iouh_Req.io_Command == UHCMD_ISOXFER)
        armed = arm_iso(unit, chan);
    else
        armed = FALSE;
    if (!armed)
        channel_release(unit, chan);
    return armed;
}

BOOL dwc2_transfer_submit(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq)
{
    struct DWC2Channel *chan;

    if (!can_arm_now(unit, ioreq) || (chan = channel_alloc(unit)) == NULL)
    {
        AddTail(&unit->transfer_queue, &ioreq->iouh_Req.io_Message.mn_Node);
        return TRUE;
    }
    return submit_on(unit, chan, ioreq);
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

/*
 * Promote every periodic request whose frame has come round.
 *
 * The single-channel engine returned after promoting one, because one was all
 * it could hold. With several channels free, a keyboard and a mouse that fall
 * due in the same frame should both go out in that frame rather than the
 * second waiting a full interval for the first to finish.
 */
void dwc2_transfer_sof(struct DWC2Unit *unit)
{
    struct Node *node;
    ULONG now;

    dwc2_transfer_service(unit);

    now = current_frame(unit);
    if (unit->sof_log_count < 8)
    {
        unit->sof_log_count++;
        bug("[DWC2/Emu68:SCHED] SOF #%u HFNUM=%08lx frame=%lu idle=%u\n",
            unit->sof_log_count, dwc2_readl(unit->device, DWC2_HFNUM), now,
            (unsigned)dwc2_transfer_idle(unit));
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
        if (channel_alloc(unit) == NULL)
            break;
        Remove(&ioreq->iouh_Req.io_Message.mn_Node);
        ioreq->iouh_DriverPrivate1 = NULL;
        if (!dwc2_transfer_submit(unit, ioreq))
        {
            ioreq->iouh_Req.io_Error = UHIOERR_HOSTERROR;
            ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
            ReplyMsg(&ioreq->iouh_Req.io_Message);
        }
    }
    update_sof_irq(unit);
}

static void rearm_stage(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    if (chan->stage == STAGE_SETUP)
        arm_setup(unit, chan);
    else if (chan->stage == STAGE_DATA)
        arm_control_data(unit, chan);
    else if (chan->stage == STAGE_STATUS)
        arm_status(unit, chan);
    else if (chan->stage == STAGE_BULK)
        arm_bulk(unit, chan);
    else if (chan->stage == STAGE_ISO)
        finish(unit, chan, 0);      /* isochronous is never retried */
    else
        arm_interrupt(unit, chan);
}

static void retry(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    if (++chan->retries > DWC2_RETRY_LIMIT)
    {
        finish(unit, chan, UHIOERR_TIMEOUT);
        return;
    }
    /*
     * Stop the channel before waiting, and wait before re-arming.
     *
     * A NAK arrives without CHHLTD -- HCINT=0x10, bit 4 alone -- so the
     * channel is still enabled when this runs, and re-arming an enabled
     * channel is what the BCM2837 answers with XACTERR. The transaction-error
     * path above already knew this and calls reset_halted_channel() before
     * rearm_stage(); the NAK path went straight to the re-arm.
     *
     * It survived that way because kprintf used to block on the UART for
     * about nine milliseconds a line, which gave the channel all the time it
     * needed to wind down on its own. The console sink took that accidental
     * delay away and the defect became a mouse that enumerates and never
     * works (ISSUE-0076). Depending on how slow the log is, to keep the bus
     * correct, is not a contract worth having.
     */
    if (!reset_halted_channel(unit, chan))
    {
        finish(unit, chan, UHIOERR_HOSTERROR);
        return;
    }
    dwc2_delay_us(unit, chan->stage == STAGE_INT_IN ? 10000 : 1000);
    rearm_stage(unit, chan);
}

/*
 * Park a periodic IN that NAKed until its next interval.
 *
 * A NAK on an interrupt endpoint is not an error -- it is the device saying
 * "nothing yet". Holding the channel open to ask again immediately would burn
 * a channel and hammer the bus, so the request goes back on the periodic
 * queue with the frame it is next due, and the channel is released for
 * somebody else.
 */
static void park_periodic(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    struct IOUsbHWReq *ioreq = chan->request;
    ULONG interval = ioreq->iouh_Interval;

    if (interval == 0)
        interval = 1;
    if (interval > DWC2_FRAME_MASK)
        interval = DWC2_FRAME_MASK;
    ioreq->iouh_DriverPrivate1 = (APTR)(IPTR)
        ((current_frame(unit) + interval) & DWC2_FRAME_MASK);
    if (unit->periodic_log_count < 8)
    {
        unit->periodic_log_count++;
        bug("[DWC2/Emu68:SCHED] NAK #%u chan=%u frame=%lu interval=%lu "
            "due=%lu\n", unit->periodic_log_count, chan->index,
            current_frame(unit), interval,
            (ULONG)(IPTR)ioreq->iouh_DriverPrivate1);
    }
    channel_release(unit, chan);
    AddTail(&unit->periodic_queue, &ioreq->iouh_Req.io_Message.mn_Node);
    update_sof_irq(unit);
    start_next(unit);
}

/*
 * Split sequencing, and why the timed half lives in the interrupt.
 *
 * A transaction translator holds the result of a start-split for a window
 * measured in microframes. Everything else in this driver is deferred -- the
 * top half records what happened and the unit task does the work -- and that
 * is the right shape for a transfer that completes in one step. A split does
 * not: accepting the start-split and going back for the result are two steps
 * with a deadline between them, and they are taken here so that the pace
 * below means microframes rather than however long a task takes to run.
 *
 * Nothing here allocates, queues or prints. Only outcomes reach the task.
 */
static void split_arm_complete(struct DWC2Unit *unit, UBYTE index)
{
    struct DWC2Device *device = unit->device;
    ULONG hcchar;

    unit->channel[index].split = DWC2_SPLIT_COMPLETE;
    unit->channel[index].watchdog_ticks = 0;
    dwc2_writel(device, DWC2_HCSPLT(index),
        dwc2_readl(device, DWC2_HCSPLT(index)) | DWC2_HCSPLT_COMPSPLT);
    dwc2_writel(device, DWC2_HCINT(index), DWC2_HCINT_ALL);
    hcchar = dwc2_readl(device, DWC2_HCCHAR(index));
    hcchar &= ~DWC2_HCCHAR_CHDIS;
    dwc2_writel(device, DWC2_HCCHAR(index), hcchar | DWC2_HCCHAR_CHENA);
}

static void split_schedule_complete(struct DWC2Unit *unit, UBYTE index)
{
    struct DWC2Channel *chan = &unit->channel[index];
    UBYTE pace;

    switch (chan->request->iouh_Req.io_Command)
    {
        case UHCMD_CONTROLXFER: pace = DWC2_SPLIT_PACE_CTRL; break;
        case UHCMD_INTXFER:     pace = DWC2_SPLIT_PACE_INT;  break;
        default:                pace = 0;                    break;
    }
    chan->split_delay = pace;
    if (pace == 0)
    {
        unit->split_pacing &= ~(1UL << index);
        split_arm_complete(unit, index);
        return;
    }
    chan->split = DWC2_SPLIT_COMPLETE;
    chan->watchdog_ticks = 0;
    unit->split_pacing |= 1UL << index;
}

BOOL dwc2_transfer_split_irq(struct DWC2Unit *unit, UBYTE index, ULONG status)
{
    struct DWC2Channel *chan = &unit->channel[index];

    if (chan->request == NULL || chan->split == DWC2_SPLIT_NONE)
        return FALSE;

    /*
     * Start-split accepted. The signal is ACK and only ACK: XFERCOMP here
     * means the transaction finished outright, which is what a controller
     * with no real translator reports, and treating that as acceptance sends
     * a complete-split to a device that was never behind one.
     */
    if (chan->split == DWC2_SPLIT_START)
    {
        if (!(status & DWC2_HCINT_ACK) || (status & DWC2_HCINT_XFERCOMP))
            return FALSE;
        chan->split_retries = 0;
        unit->split_starts++;
        split_schedule_complete(unit, index);
        return TRUE;
    }

    /* NYET: the translator has not finished. Ask again from here, while the
     * window is still open. When the budget runs out the window has closed,
     * which is an outcome and belongs to the task. */
    if ((status & DWC2_HCINT_NYET) && !(status & DWC2_HCINT_XFERCOMP))
    {
        if (++chan->split_retries > DWC2_SPLIT_NYET_LIMIT)
            return FALSE;
        split_schedule_complete(unit, index);
        return TRUE;
    }
    return FALSE;
}

void dwc2_transfer_split_sof(struct DWC2Unit *unit)
{
    UBYTE i;

    for (i = 0; i < unit->host_channels; i++)
    {
        struct DWC2Channel *chan = &unit->channel[i];

        if (!(unit->split_pacing & (1UL << i)))
            continue;
        if (chan->request == NULL || chan->split_delay == 0)
        {
            unit->split_pacing &= ~(1UL << i);
            continue;
        }
        if (--chan->split_delay == 0)
        {
            unit->split_pacing &= ~(1UL << i);
            split_arm_complete(unit, i);
        }
    }
}

/*
 * Go back to the start-split.
 *
 * A translator that answers NAK has nothing for us and has forgotten the
 * transaction; the sequence has to begin again rather than keep asking.
 */
static void split_restart(struct DWC2Unit *unit, struct DWC2Channel *chan)
{
    struct DWC2Device *device = unit->device;
    ULONG hcchar;

    chan->split = DWC2_SPLIT_START;
    chan->split_retries = 0;
    chan->watchdog_ticks = 0;
    dwc2_writel(device, DWC2_HCSPLT(chan->index),
        dwc2_readl(device, DWC2_HCSPLT(chan->index)) &
        ~DWC2_HCSPLT_COMPSPLT);
    dwc2_writel(device, DWC2_HCINT(chan->index), DWC2_HCINT_ALL);
    hcchar = dwc2_readl(device, DWC2_HCCHAR(chan->index));
    hcchar &= ~DWC2_HCCHAR_CHDIS;
    dwc2_writel(device, DWC2_HCCHAR(chan->index),
        hcchar | DWC2_HCCHAR_CHENA);
}

/*
 * The split half of a channel interrupt.
 *
 * Returns TRUE when the interrupt belonged to the split sequencer and the
 * caller has nothing left to do; FALSE when the transaction has genuinely
 * completed and the normal completion path should run.
 */
static BOOL split_irq(struct DWC2Unit *unit, struct DWC2Channel *chan,
    ULONG status)
{
    if (chan->split == DWC2_SPLIT_NONE)
        return FALSE;
    /* The start-split handshake belongs to the interrupt half; anything still
     * in DWC2_SPLIT_START here was not an acceptance. */
    if (chan->split == DWC2_SPLIT_START)
        return FALSE;

    /*
     * A NYET that reaches this far has spent the whole budget in the
     * interrupt half, and that is not a failure. Eight complete-splits is
     * one full-speed frame, which is how long a translator's result window
     * lasts; past it the result is simply gone.
     *
     * What must not happen is calling the transfer failed. For a periodic
     * endpoint the answer is the next interval. For everything else it is the
     * start-split of the *same* transaction, in place -- restarting the
     * transfer from its SETUP would abandon a transaction the translator has
     * already accepted, and abandoning one that way is what actually wedges
     * the channel's split engine.
     */
    if ((status & DWC2_HCINT_NYET) && !(status & DWC2_HCINT_XFERCOMP))
    {
        chan->split_retries = 0;
        if (chan->stage == STAGE_INT_IN)
            park_periodic(unit, chan);
        else
            split_restart(unit, chan);
        return TRUE;
    }

    /* NAK on a complete-split: the translator dropped it, start over. */
    if ((status & DWC2_HCINT_NAK) && !(status & DWC2_HCINT_XFERCOMP))
    {
        if (chan->stage == STAGE_INT_IN)
            park_periodic(unit, chan);
        else
            split_restart(unit, chan);
        return TRUE;
    }
    return FALSE;
}

/*
 * Did the device end this transfer, or did the transaction just run out?
 *
 * USB answers with a short packet: one carrying fewer bytes than the
 * endpoint's maximum is the device saying there is no more. The test is
 * against MaxPktSize and deliberately not against however much this arming
 * asked for, because those are different questions and behind a translator
 * they give different answers.
 *
 * A split moves one packet per transaction. An eighteen-byte descriptor read
 * from a low-speed device therefore comes back eight bytes at a time, three
 * times, each one reported as XFERCOMP with packets still outstanding in
 * HCTSIZ. Reading the first eight as "that is all there was" truncates every
 * descriptor such a device owns -- which is why a full-speed mouse enumerated
 * and a low-speed one did not.
 */
static BOOL device_ended_transfer(const struct IOUsbHWReq *ioreq, ULONG moved)
{
    ULONG mps = ioreq->iouh_MaxPktSize;

    if (mps == 0)
        return TRUE;
    return moved == 0 || (moved % mps) != 0;
}

/* One channel's completion. `status` is the HCINT the ISR took, or read here
 * if the wake came from the watchdog. */
static void channel_irq(struct DWC2Unit *unit, struct DWC2Channel *chan,
    ULONG status)
{
    struct DWC2Device *device = unit->device;
    struct IOUsbHWReq *ioreq = chan->request;
    ULONG remaining;
    ULONG moved;

    if (ioreq == NULL)
        return;
    if (*DWC2_LOG_BUDGET(unit, ioreq) < DWC2_TRANSFER_LOG_LIMIT)
    {
        (*DWC2_LOG_BUDGET(unit, ioreq))++;
        unit->transfer_log_count++;
        bug("[DWC2/Emu68:XFER] irq #%u chan=%u stage=%u HCINT=%08lx "
            "HCTSIZ=%08lx HCCHAR=%08lx HFNUM=%08lx\n",
            unit->transfer_log_count, chan->index, chan->stage, status,
            dwc2_readl(device, DWC2_HCTSIZ(chan->index)),
            dwc2_readl(device, DWC2_HCCHAR(chan->index)),
            dwc2_readl(device, DWC2_HFNUM));
    }
    /*
     * Isochronous has no handshake, so most of what would be an error
     * elsewhere is simply a lost interval here. Report what moved and let the
     * stream continue; only a bus-level fault (AHB, babble) is fatal.
     */
    if (chan->stage == STAGE_ISO &&
        (status & (DWC2_HCINT_XACTERR | DWC2_HCINT_FRMOVRUN |
                   DWC2_HCINT_DATATGLERR)))
    {
        finish(unit, chan, 0);
        return;
    }
    /*
     * A transaction error is an event on the wire, not an answer from the
     * device. CRC failures, bit-stuff errors, false EOPs and response
     * timeouts all arrive as XACTERR, and USB treats none of them as final:
     * the host makes three attempts before it may call a transfer failed.
     * Believing the first one turns any single glitch into a failed
     * enumeration -- and a bus that glitches once while a hub's descriptors
     * are being read is an ordinary bus, not a broken one.
     *
     * The error arrives with the channel already halted, so it needs the same
     * scrub as any other halt before it can be reprogrammed.
     */
    if ((status & DWC2_HCINT_XACTERR) &&
        ++chan->xact_errors < DWC2_XACT_ERROR_LIMIT &&
        reset_halted_channel(unit, chan))
    {
        bug("[DWC2/Emu68:XFER] xacterr chan=%u stage=%u attempt %u of %u\n",
            chan->index, chan->stage, chan->xact_errors + 1,
            DWC2_XACT_ERROR_LIMIT);
        rearm_stage(unit, chan);
        return;
    }
    if (status & (DWC2_HCINT_STALL | DWC2_HCINT_AHBERR |
                  DWC2_HCINT_XACTERR | DWC2_HCINT_BBLERR |
                  DWC2_HCINT_DATATGLERR))
    {
        /* Say which transfer died. "chan=0 stage=3" names neither the device
         * nor the request, and the arm/irq trace it would have to be read
         * against is capped well before a failure this late in a boot. */
        bug("[DWC2/Emu68:XFER] error chan=%u stage=%u HCINT=%08lx cmd=%u "
            "addr=%u ep=%u len=%lu HPRT=%08lx GINT=%08lx\n",
            chan->index, chan->stage, status, ioreq->iouh_Req.io_Command,
            ioreq->iouh_DevAddr, ioreq->iouh_Endpoint, ioreq->iouh_Length,
            dwc2_readl(device, DWC2_HPRT), dwc2_readl(device, DWC2_GINTSTS));
        finish(unit, chan, (status & DWC2_HCINT_STALL) ? UHIOERR_STALL
                                                       : UHIOERR_HOSTERROR);
        return;
    }
    /* A split transaction is two hardware transactions and most of its
     * interrupts are sequencing, not completion. Let the sequencer look
     * first; it returns FALSE only when the data has genuinely arrived. */
    if (split_irq(unit, chan, status))
        return;
    if ((status & DWC2_HCINT_NAK) && !(status & DWC2_HCINT_XFERCOMP))
    {
        if (chan->stage == STAGE_INT_IN)
            park_periodic(unit, chan);
        else
            retry(unit, chan);
        return;
    }
    if ((status & DWC2_HCINT_CHHLTD) && !(status & DWC2_HCINT_XFERCOMP))
    {
        if (!reset_halted_channel(unit, chan))
        {
            finish(unit, chan, UHIOERR_HOSTERROR);
            return;
        }
        retry(unit, chan);
        return;
    }
    if (!(status & DWC2_HCINT_XFERCOMP))
        return;

    chan->retries = 0;
    chan->xact_errors = 0;
    remaining = dwc2_readl(device, DWC2_HCTSIZ(chan->index)) & 0x7ffff;
    moved = chan->armed_length >= remaining ? chan->armed_length - remaining
                                            : 0;

    if (chan->stage == STAGE_SETUP)
    {
        if (ioreq->iouh_Length != 0)
        {
            if (!arm_control_data(unit, chan))
                finish(unit, chan, UHIOERR_HOSTERROR);
        }
        else if (!arm_status(unit, chan))
            finish(unit, chan, UHIOERR_HOSTERROR);
    }
    else if (chan->stage == STAGE_DATA)
    {
        if (ioreq->iouh_SetupData.bmRequestType & URTF_IN)
        {
            CacheClearE(chan->buffer, moved, CACRF_InvalidateD);
            CopyMem(chan->buffer,
                (UBYTE *)ioreq->iouh_Data + ioreq->iouh_Actual, moved);
        }
        ioreq->iouh_Actual += moved;
        /*
         * A stage that ran out of transaction is not a finished stage.
         *
         * This used to assign iouh_Actual and go straight to the status
         * stage, on the assumption that a control data stage always fits one
         * arming. Behind a translator it does not: a split carries at most
         * 188 bytes, so a 256-byte string descriptor came back as 184 and was
         * reported complete. Poseidon cannot parse a truncated string and
         * falls back to naming a device by its vendor and product id, which
         * is what this defect looks like from the desktop.
         *
         * Carry on while the caller's buffer is unsatisfied and the device
         * has not ended the transfer itself -- a packet shorter than the one
         * asked for is how it says there was no more.
         */
        if (ioreq->iouh_Actual < ioreq->iouh_Length &&
            !device_ended_transfer(ioreq, moved))
        {
            if (!arm_control_data(unit, chan))
                finish(unit, chan, UHIOERR_HOSTERROR);
        }
        else if (!arm_status(unit, chan))
            finish(unit, chan, UHIOERR_HOSTERROR);
    }
    else if (chan->stage == STAGE_STATUS)
        finish(unit, chan, 0);
    else if (chan->stage == STAGE_ISO)
    {
        BOOL input = (ioreq->iouh_Dir == UHDIR_IN);

        if (input && moved != 0)
        {
            CacheClearE(chan->buffer, moved, CACRF_InvalidateD);
            CopyMem(chan->buffer,
                (UBYTE *)ioreq->iouh_Data + ioreq->iouh_Actual, moved);
        }
        ioreq->iouh_Actual += moved;
        /* A short interval does not end an isochronous request the way it
         * ends a bulk one -- there is no "that is all there was", only "that
         * is all that fitted in this frame". Keep going until the caller's
         * buffer is satisfied. */
        if (ioreq->iouh_Actual >= ioreq->iouh_Length)
            finish(unit, chan, 0);
        else if (!arm_iso(unit, chan))
            finish(unit, chan, 0);
    }
    else if (chan->stage == STAGE_BULK)
    {
        BOOL input = (ioreq->iouh_Dir == UHDIR_IN);

        if (input && moved != 0)
        {
            CacheClearE(chan->buffer, moved, CACRF_InvalidateD);
            CopyMem(chan->buffer,
                (UBYTE *)ioreq->iouh_Data + ioreq->iouh_Actual, moved);
        }
        ioreq->iouh_Actual += moved;
        toggle_flip(unit, ioreq, input);

        /*
         * A short packet ends a bulk transfer whatever is left of
         * iouh_Length -- that is how a device says "this is all there was".
         * Otherwise keep going until the request is satisfied. What counts as
         * short is a question about MaxPktSize, not about this arming; see
         * device_ended_transfer().
         */
        if (device_ended_transfer(ioreq, moved) ||
            ioreq->iouh_Actual >= ioreq->iouh_Length)
            finish(unit, chan, 0);
        else if (!arm_bulk(unit, chan))
            finish(unit, chan, UHIOERR_HOSTERROR);
    }
    else
    {
        ULONG address = ioreq->iouh_DevAddr & 0x7f;

        CacheClearE(chan->buffer, moved, CACRF_InvalidateD);
        CopyMem(chan->buffer, ioreq->iouh_Data, moved);
        ioreq->iouh_Actual = moved;
        if (unit->interrupt_log_count[address] < 16)
        {
            unit->interrupt_log_count[address]++;
            bug("[DWC2/Emu68] interrupt data #%u chan=%u addr=%lu ep=%u "
                "bytes=%lu interval=%u data=%02x %02x %02x %02x %02x %02x\n",
                unit->interrupt_log_count[address], chan->index,
                address, ioreq->iouh_Endpoint, moved, ioreq->iouh_Interval,
                moved > 0 ? chan->buffer[0] : 0,
                moved > 1 ? chan->buffer[1] : 0,
                moved > 2 ? chan->buffer[2] : 0,
                moved > 3 ? chan->buffer[3] : 0,
                moved > 4 ? chan->buffer[4] : 0,
                moved > 5 ? chan->buffer[5] : 0);
        }
        toggle_flip(unit, ioreq, TRUE);
        finish(unit, chan, 0);
    }
}

/*
 * Service every channel the ISR flagged.
 *
 * channels_pending is a bitmap of channel numbers -- HAINT's own shape -- and
 * each channel carries the HCINT bits the ISR took from it. The single-channel
 * engine kept one set of HCINT bits on the unit, which cannot distinguish two
 * channels completing between two wakes.
 */
void dwc2_transfer_irq(struct DWC2Unit *unit)
{
    ULONG flagged = unit->channels_pending;
    UBYTE i;

    unit->channels_pending = 0;
    for (i = 0; i < unit->host_channels; i++)
    {
        struct DWC2Channel *chan = &unit->channel[i];
        ULONG status;

        if (!(flagged & (1UL << i)))
            continue;
        status = chan->pending;
        chan->pending = 0;
        if (status == 0)
            continue;
        channel_irq(unit, chan, status);
    }
}

/*
 * The watchdog exists for one failure only: the channel reached a terminal
 * HCINT state and the interrupt never arrived. It reads the register itself,
 * and if there is something there it feeds it through the normal path rather
 * than inventing a recovery. Only a channel that is genuinely silent for the
 * whole budget is failed.
 */
void dwc2_transfer_watchdog(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;
    UBYTE i;

    /* The watchdog is normally called every 10 ms while at least one channel
     * owns a request. Keep this deliberately sparse: the serial line can then
     * prove whether the unit task is still making progress without becoming
     * part of the scheduling problem under investigation. */
    if (++unit->watchdog_heartbeat_ticks >= 500)
    {
        unit->watchdog_heartbeat_ticks = 0;
        unit->watchdog_heartbeats++;
        bug("[DWC2/Emu68:HB] %lu AHB=%08lx irq=%08lx channels=%08lx "
            "recoveries=%lu\n", unit->watchdog_heartbeats,
            dwc2_readl(device, DWC2_GAHBCFG), unit->irq_pending,
            unit->channels_pending, unit->watchdog_recoveries);
    }

    /* A clear global mask means the ISR has already claimed an event and the
     * unit task has not consumed that snapshot yet. Treating HCINT as missed
     * in this state races the normal deferred path and creates an unbounded
     * recovery loop. The task drains before rearming this timer. */
    if (!(dwc2_readl(device, DWC2_GAHBCFG) &
        DWC2_GAHBCFG_GLBLINTRMSK))
        return;

    for (i = 0; i < unit->host_channels; i++)
    {
        struct DWC2Channel *chan = &unit->channel[i];
        ULONG status;

        if (chan->request == NULL)
            continue;
        status = dwc2_readl(device, DWC2_HCINT(chan->index));
        if (unit->watchdog_log_count < 8)
        {
            unit->watchdog_log_count++;
            bug("[DWC2/Emu68:WD] #%u chan=%u stage=%u GINT=%08lx/%08lx "
                "HAINT=%08lx/%08lx HCINT=%08lx/%08lx CHAR=%08lx "
                "TSIZ=%08lx DMA=%08lx NPTX=%08lx HFNUM=%08lx\n",
                unit->watchdog_log_count, chan->index, chan->stage,
                dwc2_readl(device, DWC2_GINTSTS),
                dwc2_readl(device, DWC2_GINTMSK),
                dwc2_readl(device, DWC2_HAINT),
                dwc2_readl(device, DWC2_HAINTMSK), status,
                dwc2_readl(device, DWC2_HCINTMSK(chan->index)),
                dwc2_readl(device, DWC2_HCCHAR(chan->index)),
                dwc2_readl(device, DWC2_HCTSIZ(chan->index)),
                dwc2_readl(device, DWC2_HCDMA(chan->index)),
                dwc2_readl(device, DWC2_GNPTXSTS),
                dwc2_readl(device, DWC2_HFNUM));
        }
        if (status != 0)
        {
            /*
             * A recovery: the channel reached a terminal state and its
             * interrupt never arrived.
             *
             * Counted separately from the detailed log above, which stops
             * after eight lines. That cap is why "8 watchdog recoveries
             * during enumeration" was recorded as a measurement when it was
             * only the log filling up -- the real number was never known.
             * Report it on the powers of two so a handful stays quiet and a
             * storm cannot hide.
             */
            unit->watchdog_recoveries++;
            if (unit->watchdog_recoveries >= unit->watchdog_reported * 2 ||
                unit->watchdog_reported == 0)
            {
                unit->watchdog_reported = unit->watchdog_recoveries;
                bug("[DWC2/Emu68:WD] %lu recoveries chan=%u stage=%u "
                    "AHB=%08lx GINT=%08lx/%08lx HAINT=%08lx/%08lx "
                    "HCINT=%08lx/%08lx\n",
                    unit->watchdog_recoveries, chan->index, chan->stage,
                    dwc2_readl(device, DWC2_GAHBCFG),
                    dwc2_readl(device, DWC2_GINTSTS),
                    dwc2_readl(device, DWC2_GINTMSK),
                    dwc2_readl(device, DWC2_HAINT),
                    dwc2_readl(device, DWC2_HAINTMSK), status,
                    dwc2_readl(device, DWC2_HCINTMSK(chan->index)));
            }
            dwc2_writel(device, DWC2_HCINT(chan->index), status);
            channel_irq(unit, chan, status);
            continue;
        }
        if (++chan->watchdog_ticks >= 100)
        {
            bug("[DWC2/Emu68:WD] chan=%u timed out stage=%u CHAR=%08lx\n",
                chan->index, chan->stage,
                dwc2_readl(device, DWC2_HCCHAR(chan->index)));
            abort_channel(unit, chan);
        }
    }
}
