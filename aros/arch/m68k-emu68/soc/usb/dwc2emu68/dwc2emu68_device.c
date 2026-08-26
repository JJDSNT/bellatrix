/*
 * Copyright (C) 2026, The Bellatrix Project.
 *
 * Device entry points for the Bellatrix DWC2 host controller.
 */

#define DEBUG 1
#include <aros/debug.h>
#include <aros/libcall.h>
#include <aros/symbolsets.h>

#include <exec/errors.h>
#include <exec/memory.h>

#include <proto/alib.h>
#include <proto/exec.h>

#include "dwc2emu68_intern.h"

const char devname[] = MOD_NAME_STRING;

static int DWC2_FNAME(Init)(LIBBASETYPEPTR DWC2Base)
{
    struct DWC2Unit *unit;
    ULONG channel;

    if (!dwc2_platform_probe(DWC2Base))
        return FALSE;

    DWC2Base->utility_base = OpenLibrary("utility.library", 39);
    if (DWC2Base->utility_base == NULL)
        return FALSE;

    unit = AllocMem(sizeof(*unit), MEMF_PUBLIC | MEMF_CLEAR);
    if (unit == NULL)
        return FALSE;

    unit->device = DWC2Base;
    NewList(&unit->transfer_queue);
    NewList(&unit->periodic_queue);
    /*
     * One bounce buffer per channel, not one per unit.
     *
     * Channels run concurrently, so a shared buffer would have two transfers
     * writing over each other -- and the cache maintenance around it would be
     * invalidating one channel's data while another was still using it. The
     * buffers are cache-line aligned because CacheClearE() works in lines and
     * would otherwise touch a neighbour's bytes.
     *
     * host_channels is not known until the controller starts, so allocate for
     * the architectural maximum; the array is small and the alternative is a
     * second allocation path at a point where failing is much more awkward.
     */
    unit->dma_length = 16384;
    for (channel = 0; channel < DWC2_MAX_CHANNELS; channel++)
    {
        struct DWC2Channel *chan = &unit->channel[channel];

        chan->index = channel;
        chan->buffer_size = unit->dma_length;
        chan->buffer_raw = AllocMem(chan->buffer_size + 63,
            MEMF_PUBLIC | MEMF_CLEAR);
        if (chan->buffer_raw == NULL)
        {
            while (channel-- > 0)
                FreeMem(unit->channel[channel].buffer_raw,
                    unit->channel[channel].buffer_size + 63);
            FreeMem(unit, sizeof(*unit));
            return FALSE;
        }
        chan->buffer = (UBYTE *)(((IPTR)chan->buffer_raw + 63) & ~(IPTR)63);
    }
    dwc2_platform_cpu0_mask(unit);
    unit->task = NewCreateTask(
        TASKTAG_NAME, "Bellatrix DWC2 unit",
        TASKTAG_PRI, -5,
        TASKTAG_AFFINITY, &unit->affinity,
        TASKTAG_PC, DWC2_FNAME(UnitTask),
        TASKTAG_TASKMSGPORT, &unit->port,
        TASKTAG_ARG1, unit,
        TAG_DONE);
    if (unit->task == NULL || unit->port == NULL)
    {
        FreeMem(unit, sizeof(*unit));
        return FALSE;
    }
    unit->soft_irq.is_Node.ln_Type = NT_INTERRUPT;
    unit->soft_irq.is_Node.ln_Name = "Bellatrix DWC2 deferred IRQ";
    unit->soft_irq.is_Node.ln_Pri = 0;
    unit->soft_irq.is_Data = unit;
    unit->soft_irq.is_Code = (VOID_FUNC)DWC2_FNAME(SoftIRQ);

    DWC2Base->unit = unit;
    bug("[DWC2/Emu68] private host-controller driver initialized\n");
    return TRUE;
}

static int DWC2_FNAME(Open)(LIBBASETYPEPTR DWC2Base,
    struct IOUsbHWReq *ioreq, ULONG unit_number, ULONG flags)
{
    struct DWC2Unit *unit = DWC2Base->unit;

    (void)flags;
    ioreq->iouh_Req.io_Error = IOERR_OPENFAIL;
    if (unit_number != 0 || unit == NULL || unit->opened ||
        ioreq->iouh_Req.io_Message.mn_Length < sizeof(*ioreq))
        return FALSE;

    unit->opened = TRUE;
    unit->unit.unit_OpenCnt++;
    ioreq->iouh_Req.io_Unit = (struct Unit *)unit;
    ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    ioreq->iouh_Req.io_Error = 0;
    ioreq->iouh_DriverPrivate2 = NULL;
    bug("[DWC2/Emu68] unit 0 opened\n");
    return TRUE;
}

static int DWC2_FNAME(Close)(LIBBASETYPEPTR DWC2Base,
    struct IOUsbHWReq *ioreq)
{
    struct DWC2Unit *unit = (struct DWC2Unit *)ioreq->iouh_Req.io_Unit;

    (void)DWC2Base;
    if (unit != NULL && unit->opened)
    {
        unit->opened = FALSE;
        unit->unit.unit_OpenCnt--;
    }
    ioreq->iouh_Req.io_Unit = (APTR)-1;
    ioreq->iouh_Req.io_Device = (APTR)-1;
    return TRUE;
}

ADD2INITLIB(DWC2_FNAME(Init), 0)
ADD2OPENDEV(DWC2_FNAME(Open), 0)
ADD2CLOSEDEV(DWC2_FNAME(Close), 0)

AROS_LH1(void, DWC2_FNAME(BeginIO),
    AROS_LHA(struct IOUsbHWReq *, ioreq, A1),
    LIBBASETYPEPTR, DWC2Base, 5, dwc2emu68)
{
    AROS_LIBFUNC_INIT

    struct DWC2Unit *unit = (struct DWC2Unit *)ioreq->iouh_Req.io_Unit;

    (void)DWC2Base;
    ioreq->iouh_Req.io_Error = 0;
    ioreq->iouh_Req.io_Flags &= (UBYTE)~IOF_QUICK;
    ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    PutMsg(unit->port, &ioreq->iouh_Req.io_Message);

    AROS_LIBFUNC_EXIT
}

AROS_LH1(LONG, DWC2_FNAME(AbortIO),
    AROS_LHA(struct IOUsbHWReq *, ioreq, A1),
    LIBBASETYPEPTR, DWC2Base, 6, dwc2emu68)
{
    AROS_LIBFUNC_INIT

    struct DWC2Unit *unit = (struct DWC2Unit *)ioreq->iouh_Req.io_Unit;

    (void)DWC2Base;
    ioreq->iouh_DriverPrivate2 = (APTR)1;
    if (unit != NULL && unit->task != NULL)
        Signal(unit->task, 1UL << unit->port->mp_SigBit);
    return 0;

    AROS_LIBFUNC_EXIT
}
