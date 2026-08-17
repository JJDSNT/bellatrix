/* Copyright (C) 2026, The Bellatrix Project. All rights reserved. */

#ifndef BTUART_PRIVATE_H
#define BTUART_PRIVATE_H

#include <exec/nodes.h>
#include <exec/semaphores.h>
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
};

#endif /* BTUART_PRIVATE_H */
