// src/host/raspi3/pal_debug.c
//
// PAL debug output — wraps Emu68's kprintf so we reuse its
// already-configured UART without reinitialising it.

#include "pal.h"
#include <stdint.h>

// Emu68 provides kprintf; declared in support.h which is on the include path.
#include "support.h"

void PAL_Debug_Init(uint32_t baud)
{
    (void)baud;
    // UART already initialised by Emu68 startup code.
}

void PAL_Debug_PutC(char c)
{
    kprintf("%c", c);
}

void PAL_Debug_Print(const char *str)
{
    kprintf("%s", str);
}

void PAL_Debug_PrintHex(uint32_t val)
{
    kprintf("%08x", val);
}
