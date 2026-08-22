/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: HCI transport for the Raspberry Pi's onboard controller.
*/

#ifndef BTHCIUART_INTERN_H
#define BTHCIUART_INTERN_H

#include <exec/devices.h>
#include <exec/semaphores.h>
#include <exec/lists.h>
#include <devices/bluetoothhci.h>

/*
 * The Bluetooth stack talks to HCI devices in DEVS:Bluetooth; upstream ships
 * one, vbthci, and it is virtual. This is the other kind: a real controller,
 * reached over the PL011 that soc/bluetooth's btuart.resource owns.
 *
 * The division of labour is the one that resource's README already states and
 * this file does not revisit. Everything below the HCI boundary -- pin muxing,
 * the 32.768 kHz LPO on GPCLK2, BT_REG_EN, baud rate, flow control -- belongs
 * to btuart.resource. Everything here is H4 framing and the AROS device
 * protocol, and it would work over any transport that can move bytes.
 *
 * H4 is the UART transport encoding from the Bluetooth spec: one type byte,
 * then the packet. It is the whole difference between what the stack hands
 * down and what goes on the wire.
 */

#define BTHCIUART_H4_COMMAND    0x01
#define BTHCIUART_H4_ACL        0x02
#define BTHCIUART_H4_SCO        0x03
#define BTHCIUART_H4_EVENT      0x04

/*
 * 115200 8N1 with RTS/CTS.
 *
 * What a BCM43438 answers to from reset, and it stays there until a firmware
 * upload changes it. Bellatrix does not upload firmware -- the Broadcom
 * patchram protocol is deliberately outside this resource's scope -- so the
 * higher rates the controller supports afterwards are not reachable yet and
 * naming one here would be a guess about a state we never enter.
 */
#define BTHCIUART_BAUD          115200

#define BTHCIUART_RXBUF         4096

struct BTHCIUARTBase
{
    struct Device            hu_Device;
    struct SignalSemaphore   hu_Lock;
    struct BTHCIUARTUnit    *hu_Unit;
    struct Library          *hu_BTUARTBase;
};

struct BTHCIUARTUnit
{
    struct Unit              hu_Unit;
    struct BTHCIUARTBase    *hu_Base;
    struct Task             *hu_Task;
    struct Task             *hu_ReadySigTask;
    BYTE                     hu_ReadySignal;
    BOOL                     hu_Open;

    /* Requests waiting for something to arrive from the controller. */
    struct MinList           hu_EventQueue;
    struct MinList           hu_ACLQueue;
    struct SignalSemaphore   hu_QueueLock;

    /* Ports registered through BTCMD_ADDMSGPORT, told about every event. */
    struct MinList           hu_Listeners;

    /* Reassembly. A UART delivers bytes, not packets. */
    UBYTE                    hu_RX[BTHCIUART_RXBUF];
    ULONG                    hu_RXLen;
};

struct BTHCIUARTListener
{
    struct MinNode           hl_Node;
    struct MsgPort          *hl_Port;
};

void bthciuart_UnitTask(void);
LONG bthciuart_QueueRequest(struct BTHCIUARTUnit *unit, struct IOBTHCIReq *ioreq);

#endif /* BTHCIUART_INTERN_H */
