/*
 * Copyright (C) 2026, The Bellatrix Project.
 *
 * Virtual one-port root hub presented to Poseidon.
 */

#include <aros/debug.h>
#include <aros/macros.h>

#include <devices/usb_hub.h>
#include <proto/exec.h>

#include "dwc2emu68_intern.h"
#include "dwc2emu68_regs.h"

#if AROS_BIG_ENDIAN
#define DWC2_CONST_LE16(v) ((((v) & 0xff) << 8) | (((v) >> 8) & 0xff))
#else
#define DWC2_CONST_LE16(v) (v)
#endif

struct DWC2RootConfig
{
    struct UsbStdCfgDesc config;
    struct UsbStdIfDesc interface;
    struct UsbStdEPDesc endpoint;
} __attribute__((packed));

static const struct UsbStdDevDesc root_device =
{
    sizeof(struct UsbStdDevDesc), UDT_DEVICE, DWC2_CONST_LE16(0x0200),
    HUB_CLASSCODE, 0, 0, 8, 0, 0, DWC2_CONST_LE16(0x0100),
    0, 1, 0, 1
};

static const struct DWC2RootConfig root_config =
{
    { sizeof(struct UsbStdCfgDesc), UDT_CONFIGURATION,
      DWC2_CONST_LE16(sizeof(struct DWC2RootConfig)), 1, 1, 0,
      USCAF_ONE | USCAF_SELF_POWERED, 0 },
    { sizeof(struct UsbStdIfDesc), UDT_INTERFACE, 0, 0, 1,
      HUB_CLASSCODE, 0, 0, 0 },
    { sizeof(struct UsbStdEPDesc), UDT_ENDPOINT, URTF_IN | 1,
      USEAF_INTERRUPT, DWC2_CONST_LE16(8), 12 }
};

static const struct UsbHubDesc root_hub =
{
    9, UDT_HUB, 1, 0, 0, 0, 0, 0xff
};

static ULONG hprt_clean(ULONG value)
{
    /* PRTENA is status on read but write-one-to-disable on write. */
    return value & ~(DWC2_HPRT_CHANGE_BITS | DWC2_HPRT_ENA);
}

static void copy_descriptor(struct IOUsbHWReq *ioreq, const void *source,
    ULONG size, ULONG requested)
{
    ioreq->iouh_Actual = requested < size ? requested : size;
    CopyMem((APTR)source, ioreq->iouh_Data, ioreq->iouh_Actual);
}

static BYTE port_reset(struct DWC2Unit *unit)
{
    struct DWC2Device *device = unit->device;
    ULONG value = dwc2_readl(device, DWC2_HPRT);

    dwc2_writel(device, DWC2_HPRT, hprt_clean(value) | DWC2_HPRT_RST);
    if (!dwc2_delay_us(unit, 60000))
        return UHIOERR_TIMEOUT;
    value = dwc2_readl(device, DWC2_HPRT);
    dwc2_writel(device, DWC2_HPRT, hprt_clean(value) & ~DWC2_HPRT_RST);
    if (!dwc2_delay_us(unit, 20000))
        return UHIOERR_TIMEOUT;
    bug("[DWC2/Emu68:RH] port reset complete HPRT=%08lx\n",
        dwc2_readl(device, DWC2_HPRT));
    unit->port_changed = TRUE;
    return 0;
}

BYTE dwc2_root_control(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq)
{
    UBYTE type = ioreq->iouh_SetupData.bmRequestType;
    UBYTE request = ioreq->iouh_SetupData.bRequest;
    UWORD value = AROS_LE2WORD(ioreq->iouh_SetupData.wValue);
    UWORD index = AROS_LE2WORD(ioreq->iouh_SetupData.wIndex);
    UWORD length = AROS_LE2WORD(ioreq->iouh_SetupData.wLength);
    ULONG hprt;

    bug("[DWC2/Emu68:RH] ctrl type=%02lx req=%02lx value=%04lx index=%04lx len=%lu\n",
        (ULONG)type, (ULONG)request, (ULONG)value, (ULONG)index,
        (ULONG)length);
    ioreq->iouh_Actual = 0;
    if (length != ioreq->iouh_Length)
        return UHIOERR_STALL;

    if (type == (URTF_STANDARD | URTF_DEVICE))
    {
        if (request == USR_SET_ADDRESS)
        {
            unit->hub_address = value & 0x7f;
            return 0;
        }
        if (request == USR_SET_CONFIGURATION)
            return 0;
    }
    else if (type == (URTF_IN | URTF_STANDARD | URTF_DEVICE))
    {
        if (request == USR_GET_DESCRIPTOR)
        {
            switch (value >> 8)
            {
                case UDT_DEVICE:
                    copy_descriptor(ioreq, &root_device, sizeof(root_device), length);
                    return 0;
                case UDT_CONFIGURATION:
                    copy_descriptor(ioreq, &root_config, sizeof(root_config), length);
                    return 0;
                case UDT_STRING:
                    if (length >= 4 && (value & 0xff) == 0)
                    {
                        UBYTE *out = ioreq->iouh_Data;
                        out[0] = 4; out[1] = UDT_STRING;
                        out[2] = 0x09; out[3] = 0x04;
                        ioreq->iouh_Actual = 4;
                        return 0;
                    }
                    return UHIOERR_STALL;
            }
        }
        if (request == USR_GET_STATUS && length == 2)
        {
            *(UWORD *)ioreq->iouh_Data = AROS_WORD2LE(U_GSF_SELF_POWERED);
            ioreq->iouh_Actual = 2;
            return 0;
        }
        if (request == USR_GET_CONFIGURATION && length == 1)
        {
            *(UBYTE *)ioreq->iouh_Data = 1;
            ioreq->iouh_Actual = 1;
            return 0;
        }
    }
    else if (type == (URTF_IN | URTF_CLASS | URTF_DEVICE))
    {
        if (request == USR_GET_DESCRIPTOR && (value >> 8) == UDT_HUB)
        {
            copy_descriptor(ioreq, &root_hub, sizeof(root_hub), length);
            return 0;
        }
        if (request == USR_GET_STATUS && length >= sizeof(struct UsbHubStatus))
        {
            *(ULONG *)ioreq->iouh_Data = 0;
            ioreq->iouh_Actual = sizeof(struct UsbHubStatus);
            return 0;
        }
    }
    else if (type == (URTF_IN | URTF_CLASS | URTF_OTHER) &&
        request == USR_GET_STATUS && index == 1 &&
        length == sizeof(struct UsbPortStatus))
    {
        UWORD *status = ioreq->iouh_Data;
        hprt = dwc2_readl(unit->device, DWC2_HPRT);
        status[0] = 0;
        status[1] = 0;
        if (hprt & DWC2_HPRT_CONNSTS) status[0] |= AROS_WORD2LE(UPSF_PORT_CONNECTION);
        if (hprt & DWC2_HPRT_ENA) status[0] |= AROS_WORD2LE(UPSF_PORT_ENABLE);
        if (hprt & DWC2_HPRT_PWR) status[0] |= AROS_WORD2LE(UPSF_PORT_POWER);
        if (hprt & DWC2_HPRT_RST) status[0] |= AROS_WORD2LE(UPSF_PORT_RESET);
        if (hprt & DWC2_HPRT_CONNDET) status[1] |= AROS_WORD2LE(UPSF_PORT_CONNECTION);
        if (hprt & DWC2_HPRT_ENCHNG) status[1] |= AROS_WORD2LE(UPSF_PORT_ENABLE);
        ioreq->iouh_Actual = sizeof(struct UsbPortStatus);
        return 0;
    }
    else if (type == (URTF_CLASS | URTF_OTHER) && index == 1)
    {
        hprt = dwc2_readl(unit->device, DWC2_HPRT);
        if (request == USR_SET_FEATURE)
        {
            if (value == UFS_PORT_POWER)
                dwc2_writel(unit->device, DWC2_HPRT, hprt_clean(hprt) | DWC2_HPRT_PWR);
            else if (value == UFS_PORT_RESET)
                return port_reset(unit);
            else
                return UHIOERR_STALL;
            return 0;
        }
        if (request == USR_CLEAR_FEATURE)
        {
            ULONG clear = 0;
            if (value == UFS_C_PORT_CONNECTION) clear = DWC2_HPRT_CONNDET;
            else if (value == UFS_C_PORT_ENABLE) clear = DWC2_HPRT_ENCHNG;
            else if (value == UFS_C_PORT_OVER_CURRENT) clear = DWC2_HPRT_OVRCURRCHNG;
            else if (value == UFS_C_PORT_RESET) { unit->port_changed = FALSE; return 0; }
            else return UHIOERR_STALL;
            dwc2_writel(unit->device, DWC2_HPRT, hprt_clean(hprt) | clear);
            unit->port_changed = (dwc2_readl(unit->device, DWC2_HPRT) &
                DWC2_HPRT_CHANGE_BITS) != 0;
            return 0;
        }
    }
    return UHIOERR_STALL;
}

BOOL dwc2_root_interrupt(struct DWC2Unit *unit, struct IOUsbHWReq *ioreq)
{
    bug("[DWC2/Emu68:RH] interrupt ep=%lu len=%lu changed=%ld HPRT=%08lx\n",
        (ULONG)ioreq->iouh_Endpoint, (ULONG)ioreq->iouh_Length,
        (LONG)unit->port_changed, dwc2_readl(unit->device, DWC2_HPRT));
    if (ioreq->iouh_Endpoint != 1 || ioreq->iouh_Length == 0)
        return FALSE;
    if (unit->port_changed)
    {
        *(UBYTE *)ioreq->iouh_Data = 2;
        ioreq->iouh_Actual = 1;
        unit->port_changed = FALSE;
        return TRUE;
    }
    unit->hub_interrupt = ioreq;
    return FALSE;
}

void dwc2_root_poll(struct DWC2Unit *unit)
{
    struct IOUsbHWReq *ioreq = unit->hub_interrupt;

    if (ioreq != NULL && unit->port_changed)
    {
        unit->hub_interrupt = NULL;
        *(UBYTE *)ioreq->iouh_Data = 2;
        ioreq->iouh_Actual = 1;
        unit->port_changed = FALSE;
        ioreq->iouh_Req.io_Error = 0;
        ioreq->iouh_Req.io_Message.mn_Node.ln_Type = NT_FREEMSG;
        ReplyMsg(&ioreq->iouh_Req.io_Message);
    }
}
