/*
    Copyright (C) 2013-2019, The AROS Development Team. All rights reserved.
*/

#include "sdcard_intern.h"
#include "timer.h"

#include <hardware/sdhc.h>

/*
 * The data port is not a register, and must not be endian-converted.
 *
 * Every other SDHCI register is a little-endian value, so a big-endian guest
 * has to swap it -- that is what AROS_LE2LONG is doing below, and it is
 * correct there. SDHCI_BUFFER is different: it is a byte stream. Card bytes
 * b0,b1,b2,b3 arrive packed little-endian into the 32-bit port, so a plain
 * 32-bit load on this big-endian CPU already composes
 * (b0<<24)|(b1<<16)|(b2<<8)|b3, and storing that word puts b0 at the lowest
 * address -- which is exactly right. Swapping it reverses every four bytes of
 * user data, and nothing reports an error: reads "succeed" and return
 * scrambled sectors, so the MBR is never recognised and the caller retries
 * forever.
 *
 * On a little-endian host AROS_LE2LONG is a no-op, which is why the shared
 * code has always been able to use one accessor for both.
 *
 * The proper fix belongs upstream, not here: rom/devs/sdcard should carry a
 * separate pair of vtable entries for the data FIFO, distinct from the
 * register accessors, so that the distinction is made once for every board
 * and every byte order. That would also fix arch/arm-native's raspi-armeb
 * target, which is big-endian ARM and has the same bug latent in it. This is
 * the local workaround: our backend knows which offset is the data port, so
 * it special-cases it and leaves the generic skeleton untouched.
 */
static inline int sdc_is_data_port(ULONG reg)
{
    return reg == SDHCI_BUFFER;
}

void FNAME_BCMSDCBUS(BCMLEDCtrl)(int lvl)
{
    if (lvl > 0)
    {
        // Activity LED ON
        if (__arm_periiobase == BCM2835_PERIPHYSBASE)
            *(volatile unsigned int *)GPCLR0 = AROS_LONG2LE(1 << 16);
        else
            *(volatile unsigned int *)GPSET1 = AROS_LONG2LE(1 << (47 - 32));
    }
    else
    {
        // Activity LED OFF
        if (__arm_periiobase == BCM2835_PERIPHYSBASE)
            *(volatile unsigned int *)GPSET0 = AROS_LONG2LE(1 << 16);
        else
            *(volatile unsigned int *)GPCLR1 = AROS_LONG2LE(1 << (47 - 32));
    }
}

ULONG FNAME_SDCBUS(GetClockDiv)(ULONG speed, struct sdcard_Bus *bus)
{
    ULONG __BCMClkDiv;

    for (__BCMClkDiv = 0; __BCMClkDiv < V300_MAXCLKDIV; __BCMClkDiv++) {
        if ((bus->sdcb_ClockMax / (__BCMClkDiv + 1)) <= speed)
                break;
    }

    return __BCMClkDiv;
}

UBYTE FNAME_BCMSDCBUS(BCMMMIOReadByte)(ULONG reg, struct sdcard_Bus *bus)
{
    ULONG val = AROS_LE2LONG(*(volatile ULONG *)(((ULONG)bus->sdcb_IOBase + reg) & ~3));

    return (val >> ((reg & 3) << 3)) & 0xFF;
}

UWORD FNAME_BCMSDCBUS(BCMMMIOReadWord)(ULONG reg, struct sdcard_Bus *bus)
{
    ULONG val = AROS_LE2LONG(*(volatile ULONG *)(((ULONG)bus->sdcb_IOBase + reg) & ~3));

    return (val >> (((reg >> 1) & 1) << 4)) & 0xFFFF;
}

ULONG FNAME_BCMSDCBUS(BCMMMIOReadLong)(ULONG reg, struct sdcard_Bus *bus)
{
    ULONG raw = *(volatile ULONG *)(bus->sdcb_IOBase + reg);

    return sdc_is_data_port(reg) ? raw : AROS_LE2LONG(raw);
}

static void FNAME_BCMSDCBUS(BCM283xWriteLong)(ULONG reg, ULONG val, struct sdcard_Bus *bus)
{
    /* Bug: two SDC clock cycle delay required between successive chipset writes */
    while (sdcard_CurrentTime() < (bus->sdcb_Private + 6))
        sdcard_Udelay(1);

    /* Writes to the data port carry user bytes, not a register value -- see
     * the note above sdc_is_data_port(). */
    *(volatile ULONG *)(bus->sdcb_IOBase + reg) =
        sdc_is_data_port(reg) ? val : AROS_LONG2LE(val);
    bus->sdcb_Private = (IPTR)sdcard_CurrentTime();
}

void FNAME_BCMSDCBUS(BCMMMIOWriteByte)(ULONG reg, UBYTE val, struct sdcard_Bus *bus)
{
    ULONG currval = AROS_LE2LONG(*(volatile ULONG *)(((ULONG)bus->sdcb_IOBase + reg) & ~3));
    ULONG shift = (reg & 3) << 3;
    ULONG mask = 0xFF << shift;
    ULONG newval = (currval & ~mask) | (val << shift);

    FNAME_BCMSDCBUS(BCM283xWriteLong)(reg & ~3, newval, bus);
}

void FNAME_BCMSDCBUS(BCMMMIOWriteWord)(ULONG reg, UWORD val, struct sdcard_Bus *bus)
{
    ULONG currval = AROS_LE2LONG(*(volatile ULONG *)(((ULONG)bus->sdcb_IOBase + reg) & ~3));
    ULONG shift = ((reg >> 1) & 1) << 4;
    ULONG mask = 0xFFFF << shift;
    ULONG newval = (currval & ~mask) | (val << shift);

    FNAME_BCMSDCBUS(BCM283xWriteLong)(reg & ~3, newval, bus);
}

void FNAME_BCMSDCBUS(BCMMMIOWriteLong)(ULONG reg, ULONG val, struct sdcard_Bus *bus)
{
    FNAME_BCMSDCBUS(BCM283xWriteLong)(reg, val, bus);
}
