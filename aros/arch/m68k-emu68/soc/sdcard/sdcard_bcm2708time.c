/*
    Copyright (C) 2013-2019, The AROS Development Team. All rights reserved.
*/

#include "sdcard_intern.h"

/*
 * The state both backends share, defined in the file both backends build --
 * which is this one, and only this one, since the Arasan and SDHOST halves
 * are alternatives rather than companions here.
 *
 * arch/arm-native puts them here for the same reason. They lived in
 * sdcard_bcm2708init.c in this port while the Arasan backend was the only one
 * there was; that stopped being true when SDHOST arrived, which only declares
 * them extern and expects someone else to own them.
 *
 * DMABase is only opened by the SDHOST backend (dma.resource is what it needs
 * and Arasan does not), but it is defined unconditionally: a definition that
 * nothing references costs a pointer, and making it conditional would put a
 * second thing to keep in step with SDCARD_BACKEND in mmakefile.src.
 */
APTR            MBoxBase;
APTR            DMABase;
IPTR            __arm_periiobase __attribute__((used)) = 0;

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
