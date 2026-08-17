/* Copyright (C) 2026, The Bellatrix Project. All rights reserved. */

#ifndef BTUART_PRIVATE_H
#define BTUART_PRIVATE_H

#include <exec/nodes.h>
#include <exec/semaphores.h>

/*
 * Power of two, so the index arithmetic masks rather than divides.
 *
 * Sized against the traffic, not against a tick period. The BCM43430A1 does not
 * send one advertising report per event: during an LE scan it batches every
 * advert it heard into a single HCI event of up to 1220 bytes. Two of those
 * back to back overflow a 2 KB ring, and an overflow costs the H4 framer its
 * synchronisation just as an overrun does. 8 KB holds six.
 */
#define BTUART_RX_RING 8192
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
