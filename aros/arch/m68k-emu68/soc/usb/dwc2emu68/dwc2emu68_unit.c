/*
 * Copyright (C) 2026, The Bellatrix Project.
 *
 * All controller and request state is owned by this unit task. The hardware
 * interrupt will only mask/ack the source and signal this task.
 */

#include <aros/debug.h>

#include <exec/errors.h>

#include <proto/exec.h>

#include "dwc2emu68_intern.h"

#define UtilityBase unit->device->utility_base
#include <proto/utility.h>

static const UWORD supported_commands[] =
{
    CMD_FLUSH,
    CMD_RESET,
    UHCMD_QUERYDEVICE,
    UHCMD_USBRESET,
    UHCMD_USBRESUME,
    UHCMD_USBSUSPEND,
    UHCMD_USBOPER,
    UHCMD_CONTROLXFER,
    UHCMD_INTXFER,
    UHCMD_BULKXFER,
    /*
     * UHCMD_ISOXFER is listed because handle_request() now implements it.
     *
     * This table is a promise about what the switch does. It once listed
     * transfers the switch answered with IOERR_NOCMD, which is a lie to
     * Poseidon; it then listed neither, which was honest and incomplete. It
     * lists them now because arm_iso() exists.
     */
    UHCMD_ISOXFER,
    NSCMD_DEVICEQUERY,
    0
};

static void finish_request(struct IOUsbHWReq *ioreq, BYTE error)
{
    ioreq->iouh_Req.io_Error = error;
    ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
    if (!(ioreq->iouh_Req.io_Flags & IOF_QUICK))
        ReplyMsg(&ioreq->iouh_Req.io_Message);
}

static BYTE query_newstyle(struct IOUsbHWReq *ioreq)
{
    struct IOStdReq *stdreq = (struct IOStdReq *)ioreq;
    struct DWC2NSQueryResult *query = stdreq->io_Data;

    if (query == NULL || stdreq->io_Length < sizeof(*query) ||
        query->DevQueryFormat != 0 || query->SizeAvailable != 0)
        return IOERR_NOCMD;

    stdreq->io_Actual = query->SizeAvailable = sizeof(*query);
    query->DeviceType = NSDEVTYPE_USBHARDWARE;
    query->DeviceSubType = 0;
    query->SupportedCommands = supported_commands;
    return 0;
}

static BYTE query_device(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq)
{
    struct TagItem *tags = (struct TagItem *)ioreq->iouh_Data;
    struct TagItem *tag;
    ULONG count = 0;

    while ((tag = NextTagItem(&tags)) != NULL)
    {
        switch (tag->ti_Tag)
        {
            case UHA_State: *(IPTR *)tag->ti_Data = UHSF_OPERATIONAL; break;
            case UHA_Manufacturer: *(STRPTR *)tag->ti_Data = "Bellatrix Project"; break;
            case UHA_ProductName: *(STRPTR *)tag->ti_Data = "Emu68 DWC2 Host Controller"; break;
            case UHA_Description: *(STRPTR *)tag->ti_Data = "Bellatrix native Synopsys DWC2 driver"; break;
            case UHA_Copyright: *(STRPTR *)tag->ti_Data = "2026 Bellatrix Project"; break;
            case UHA_Version: *(IPTR *)tag->ti_Data = 0; break;
            case UHA_Revision: *(IPTR *)tag->ti_Data = 1; break;
            case UHA_DriverVersion: *(IPTR *)tag->ti_Data = 0x0001; break;
            case UHA_Capabilities: *(IPTR *)tag->ti_Data = UHCF_USB20; break;
            default: continue;
        }
        count++;
    }
    ioreq->iouh_Actual = count;
    return 0;
}

static void process_request(struct IOUsbHWReq *ioreq)
{
    struct DWC2Unit *unit = (struct DWC2Unit *)ioreq->iouh_Req.io_Unit;
    BYTE error;
    BOOL reply = TRUE;

    if (ioreq->iouh_DriverPrivate2 != NULL)
    {
        finish_request(ioreq, IOERR_ABORTED);
        return;
    }

    if (!dwc2_controller_start(unit))
    {
        finish_request(ioreq, IOERR_OPENFAIL);
        return;
    }

    switch (ioreq->iouh_Req.io_Command)
    {
        case NSCMD_DEVICEQUERY:
            error = query_newstyle(ioreq);
            break;

        case UHCMD_QUERYDEVICE:
            error = query_device(unit, ioreq);
            break;

        case CMD_RESET:
        case CMD_FLUSH:
        case UHCMD_USBRESET:
        case UHCMD_USBRESUME:
        case UHCMD_USBSUSPEND:
        case UHCMD_USBOPER:
            error = 0;
            break;

        case UHCMD_CONTROLXFER:
            if (ioreq->iouh_DevAddr == unit->hub_address)
                error = dwc2_root_control(unit, ioreq);
            else
            {
                reply = !dwc2_transfer_submit(unit, ioreq);
                error = reply ? UHIOERR_HOSTERROR : 0;
            }
            break;

        case UHCMD_BULKXFER:
            /* No such thing as a bulk endpoint on the root hub. */
            reply = !dwc2_transfer_submit(unit, ioreq);
            error = reply ? UHIOERR_HOSTERROR : 0;
            break;

        /*
         * Isochronous, and note what is NOT done here: the request is never
         * queued and left unanswered. The inherited engine's cmdIsoXFer()
         * adds the request to a list, returns RC_DONTREPLY, and has the call
         * that would drain that list commented out -- so a caller waits for
         * an answer that cannot come. Refusing would have been kinder; doing
         * it is kinder still.
         */
        case UHCMD_ISOXFER:
            if (ioreq->iouh_Flags & UHFF_LOWSPEED)
            {
                error = UHIOERR_BADPARAMS;
                break;
            }
            reply = !dwc2_transfer_submit(unit, ioreq);
            error = reply ? UHIOERR_HOSTERROR : 0;
            break;

        case UHCMD_INTXFER:
            if (ioreq->iouh_DevAddr == unit->hub_address)
            {
                reply = dwc2_root_interrupt(unit, ioreq);
                error = 0;
            }
            else
            {
                reply = !dwc2_transfer_submit(unit, ioreq);
                error = reply ? UHIOERR_HOSTERROR : 0;
            }
            break;

        /* Hardware commands are enabled one by one as the native DWC2 core
         * lands. Returning an error is intentional and bounded meanwhile. */
        default:
            error = IOERR_NOCMD;
            break;
    }
    if (reply)
        finish_request(ioreq, error);
}

void DWC2_FNAME(UnitTask)(struct DWC2Unit *unit)
{
    ULONG port_signal = 1UL << unit->port->mp_SigBit;
    ULONG timer_signal;

    unit->timer_port = CreateMsgPort();
    if (unit->timer_port != NULL)
        unit->timer_request = (struct timerequest *)CreateIORequest(
            unit->timer_port, sizeof(*unit->timer_request));
    if (unit->timer_request == NULL ||
        OpenDevice(TIMERNAME, UNIT_MICROHZ,
            (struct IORequest *)unit->timer_request, 0) != 0)
    {
        bug("[DWC2/Emu68] unit task could not open timer.device\n");
        unit->timer_request = NULL;
    }
    bug("[DWC2/Emu68] unit task ready\n");
    timer_signal = unit->timer_port != NULL ?
        1UL << unit->timer_port->mp_SigBit : 0;
    for (;;)
    {
        struct IOUsbHWReq *ioreq;

        ULONG signals = Wait(port_signal | timer_signal);
        /* Prefer already-published channel completions to an expired
         * watchdog when their signals coalesce. */
        if (unit->irq_pending != 0)
            dwc2_controller_drain_irq(unit);
        if ((signals & timer_signal) && unit->watchdog_active)
        {
            WaitIO((struct IORequest *)unit->timer_request);
            unit->watchdog_active = FALSE;
            /* WaitIO() is another scheduling point. An IRQ may have closed
             * the controller after the drain above but before the timer
             * request was collected, so consume that snapshot before the
             * watchdog decides that its terminal HCINT was missed. */
            if (unit->irq_pending != 0)
                dwc2_controller_drain_irq(unit);
            dwc2_transfer_watchdog(unit);
        }
        if (unit->hub_interrupt != NULL &&
            unit->hub_interrupt->iouh_DriverPrivate2 != NULL)
        {
            ioreq = unit->hub_interrupt;
            unit->hub_interrupt = NULL;
            finish_request(ioreq, IOERR_ABORTED);
        }
        if (dwc2_transfer_pending_abort(unit))
            dwc2_transfer_abort_all(unit);
        while ((ioreq = (struct IOUsbHWReq *)GetMsg(unit->port)) != NULL)
        {
            process_request(ioreq);
        }
        dwc2_transfer_service(unit);
        /* A channel may complete while process_request() is still arming it.
         * In that case the IRQ shares/coalesces with the message-port signal;
         * check again before sleeping so the gated controller is always
         * rearmed even when no second signal edge is observed. */
        if (unit->irq_pending != 0)
        {
            dwc2_controller_drain_irq(unit);
        }
        if (!dwc2_transfer_idle(unit) && !unit->watchdog_active)
            dwc2_watchdog_arm(unit);
    }
}

void dwc2_watchdog_arm(struct DWC2Unit *unit)
{
    if (unit->timer_request == NULL || unit->watchdog_active)
        return;
    unit->timer_request->tr_node.io_Command = TR_ADDREQUEST;
    unit->timer_request->tr_time.tv_secs = 0;
    unit->timer_request->tr_time.tv_micro = 10000;
    unit->watchdog_active = TRUE;
    SendIO((struct IORequest *)unit->timer_request);
}

void dwc2_watchdog_cancel(struct DWC2Unit *unit)
{
    if (!unit->watchdog_active)
        return;
    if (!CheckIO((struct IORequest *)unit->timer_request))
        AbortIO((struct IORequest *)unit->timer_request);
    WaitIO((struct IORequest *)unit->timer_request);
    unit->watchdog_active = FALSE;
}
