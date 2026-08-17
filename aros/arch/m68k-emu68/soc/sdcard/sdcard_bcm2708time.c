/*
    Copyright (C) 2013-2019, The AROS Development Team. All rights reserved.
*/

#include "sdcard_intern.h"

/*
 * The arm-native original spells this "yield" on AArch64 and "mov r0, r0" on
 * ARM. Under Emu68 the guest is an m68k, and a plain nop is both the right
 * instruction and cheap for the JIT: it is one of the opcodes Emu68 folds
 * away, so the delay loop is paced by SYSTIMER_CLO rather than by how long
 * the spin instruction happens to take.
 */
#define NOP() asm volatile("nop\n")

ULONG sdcard_CurrentTime()
{
    return AROS_LE2LONG(*((volatile ULONG *)(SYSTIMER_CLO)));
}

void sdcard_Udelay(ULONG usec)
{
    ULONG now = sdcard_CurrentTime();
    do
    {
        NOP();
    } while (sdcard_CurrentTime() < (now + usec));
}

void sdcard_WaitNano(register ULONG ns, struct SDCardBase *SDCardBase)
{
    while (ns > 0)
    {
        NOP();
        --ns;
    }
}
