/* Copyright (C) 2026, The Bellatrix Project. All rights reserved. */

#ifndef BTUART_PRIVATE_H
#define BTUART_PRIVATE_H

#include <exec/nodes.h>
#include <exec/semaphores.h>

/* Power of two: the index arithmetic below masks rather than divides. */
#define BTUART_RX_RING 2048
#include <exec/types.h>

struct BTUARTBase
{
    struct Node node;
    struct SignalSemaphore lock;
    ULONG capabilities;
    APTR owner;
    IPTR peripheral_base;
    IPTR uart_base;
    IPTR gpio_base;
    ULONG uart_clock_hz;
    ULONG baud;
    ULONG config_flags;
    ULONG rx_errors;   /* receive status reports emitted, see BTUARTRead */

    /*
     * Receive ring, filled by the PL011 interrupt and drained by BTUARTRead().
     *
     * The FIFO holds sixteen bytes, which at 115200 baud is 1.4 ms. Nothing
     * scheduled can be relied on to visit it that often: polling at 10 ms lost
     * data on every burst and polling at 1 ms still reported OVERRUN on real
     * hardware during an LE scan. The ring is sized for a worst-case burst of
     * whole HCI packets rather than for a tick period, so a late reader costs
     * latency instead of bytes.
     */
    volatile ULONG rx_head;   /* written by the handler */
    volatile ULONG rx_tail;   /* written by BTUARTRead */
    volatile ULONG rx_dropped;
    BOOL rx_irq_armed;
    UBYTE rx_ring[BTUART_RX_RING];
};

#endif /* BTUART_PRIVATE_H */
