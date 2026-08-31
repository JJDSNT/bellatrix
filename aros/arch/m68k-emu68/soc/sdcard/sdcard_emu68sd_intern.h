#ifndef _SDCARDEMU68SD_INTERN_H
#define _SDCARDEMU68SD_INTERN_H
/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    BCM2835 SDHOST controller driver internals.
*/

#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/ports.h>
#include <utility/utility.h>
#include <exec/io.h>
#include <exec/errors.h>

#include <devices/timer.h>

extern IPTR __arm_periiobase;
#define ARM_PERIIOBASE __arm_periiobase
#include <hardware/bcm2708.h>
#include <hardware/sdhost.h>

#include "sdcard_base.h"

#define FNAME_SDHOST(x)                 SDHOST__Device__ ## x
#define FNAME_SDHOSTBUS(x)             SDHOST__SDBus__ ## x

/* Private data stored in sdcb_Private (cast to struct sdhost_private *) */
struct sdhost_private {
    ULONG   max_clk;            /* Core clock rate from VideoCore */
    ULONG   cdiv;               /* Cached SDCDIV value */
    ULONG   hcfg;               /* Cached SDHCFG value */

    /*
     * No DMA state, and that is the whole point of this backend.
     *
     * Emu68's own driver moves data through the FIFO in bursts and allocates
     * neither a channel nor a bounce buffer. Everything that made the other
     * backend need those -- alignment, bus addressability, cache maintenance
     * -- is simply absent here, and so is the copy that alignment failures
     * were paying for.
     */
    BOOL    xfer_active;
};

/* Accessor for bus private data */
#define SDHOST_PRIV(bus)  ((struct sdhost_private *)(bus)->sdcb_Private)

/*
 * Control registers are byte-swapped; the data port is not.
 *
 * Emu68's driver spells this rd32 versus rd32be and the distinction is load
 * bearing: a byte stream read as a 32-bit word on a big-endian CPU and written
 * back raw keeps its byte order, while swapping it reverses every group of
 * four. sdhost_read() must never touch SDDATA.
 */
static inline ULONG sdhost_read(struct sdcard_Bus *bus, ULONG reg)
{
    return AROS_LE2LONG(*(volatile ULONG *)((ULONG)bus->sdcb_IOBase + reg));
}

static inline void sdhost_write(struct sdcard_Bus *bus, ULONG reg, ULONG val)
{
    *(volatile ULONG *)((ULONG)bus->sdcb_IOBase + reg) = AROS_LONG2LE(val);
}

/* Function declarations */
void  FNAME_SDHOSTBUS(SoftReset)(UBYTE mask, struct sdcard_Bus *bus);
void  FNAME_SDHOSTBUS(SetClock)(ULONG speed, struct sdcard_Bus *bus);
void  FNAME_SDHOSTBUS(SetPowerLevel)(ULONG supportedlvls, BOOL lowest, struct sdcard_Bus *bus);
ULONG FNAME_SDHOSTBUS(SendCmd)(struct TagItem *CmdTags, struct sdcard_Bus *bus);
ULONG FNAME_SDHOSTBUS(WaitCmd)(ULONG mask, ULONG timeout, struct sdcard_Bus *bus);
ULONG FNAME_SDHOSTBUS(FinishCmd)(struct TagItem *CmdTags, struct sdcard_Bus *bus);
ULONG FNAME_SDHOSTBUS(FinishData)(struct TagItem *DataTags, struct sdcard_Bus *bus);
void  FNAME_SDHOSTBUS(BusIRQ)(struct sdcard_Bus *bus, void *unused);
void  FNAME_SDHOSTBUS(SetBusWidth)(UBYTE width, struct sdcard_Bus *bus);
void  FNAME_SDHOST(BusInit)(struct sdcard_Bus *bus);
void  FNAME_SDHOST(BusPostIRQInit)(struct sdcard_Bus *bus);

/* Raw, unswapped: the FIFO carries a byte stream, not a register value. */
static inline ULONG sdhost_data_read(struct sdcard_Bus *bus)
{
    return *(volatile ULONG *)((ULONG)bus->sdcb_IOBase + SDDATA);
}

static inline void sdhost_data_write(struct sdcard_Bus *bus, ULONG val)
{
    *(volatile ULONG *)((ULONG)bus->sdcb_IOBase + SDDATA) = val;
}

#endif /* _SDCARDEMU68SD_INTERN_H */
