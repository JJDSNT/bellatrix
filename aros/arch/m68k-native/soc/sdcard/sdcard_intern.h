#ifndef _SDCARDBCM2708_INTERN_H
#define _SDCARDBCM2708_INTERN_H
/*
    Copyright © 2013-2015, The AROS Development Team. All rights reserved.

    Arasan SDHCI backend for arch/m68k-emu68.

    Adapted from arch/arm-native/soc/broadcom/2708/sdcard. rom/devs/sdcard is
    the generic skeleton and is built unmodified; this directory only supplies
    the board-specific half, attached with %build_archspecific. The Arasan
    controller is what QEMU's raspi3b emulates, and unlike the SDHOST backend
    it is pure PIO -- no dma.resource needed.
*/

#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/ports.h>
#include <utility/utility.h>
#include <exec/io.h>
#include <exec/errors.h>

#include <hardware/bcm2708.h>
#include <hardware/videocore.h>
#include <devices/timer.h>

/*
 * Peripheral window base, filled in from KrnGetSystemAttr(KATTR_PeripheralBase)
 * during init -- see arch/m68k-native/kernel/getsystemattr.c, which reports
 * what platform.c discovered from /soc's "ranges". Emu68 maps the Pi
 * peripherals where it likes, so this is never a compile-time constant.
 */
extern IPTR __arm_periiobase;
#define ARM_PERIIOBASE __arm_periiobase

/*
 * arch/arm-native pulls this out of its own <hardware/bcm2708.h>. That is
 * another target's tree, and all we need from its 400-odd lines is one IRQ
 * number, so it is spelled out here the way platform/bcm283x spells out its
 * register offsets.
 *
 * Logical IRQ numbering is bank = irq >> 5, bit = irq & 0x1f, matching
 * platform/bcm283x/interrupt_controller.c: bank 1 is GPUIRQ_PEND1/ENBL1, so
 * the Arasan SDIO controller's GPU IRQ 30 in bank 1 is 32 + 30.
 */
/* From <hardware/bcm2708.h>, which is where this port keeps the BCM283x
 * interrupt map. It used to be defined here and again in the Bluetooth
 * driver -- a shared numbering held privately in two places. */

/* Free-running microsecond counter, used for the inter-write delay the
 * controller needs and for sdcard_Udelay(). Same register platform/bcm283x/
 * system_timer.c drives, read here directly rather than through the driver. */
#define SYSTIMER_BASE                   (ARM_PERIIOBASE + 0x003000)
#define SYSTIMER_CLO                    (SYSTIMER_BASE + 0x04)

/* Activity LED. On a BCM2835 it hangs off GPIO 16 (bank 0, and lit by
 * *clearing* the pin); from the BCM2836 on it is GPIO 47, in bank 1. The
 * backend picks between them by comparing the peripheral base against
 * BCM2835_PERIPHYSBASE -- on Emu68 the base is the window Emu68 chose and
 * never 0x20000000, so the bank 1 path is the one taken. */
#define BCM2835_PERIPHYSBASE            0x20000000
#define GPIO_BASE                       (ARM_PERIIOBASE + 0x200000)
#define GPSET0                          (GPIO_BASE + 0x1C)
#define GPSET1                          (GPIO_BASE + 0x20)
#define GPCLR0                          (GPIO_BASE + 0x28)
#define GPCLR1                          (GPIO_BASE + 0x2C)

#include "sdcard_base.h"

#define FNAME_BCMSDC(x)                 BCM2708SD__Device__ ## x
#define FNAME_BCMSDCBUS(x)              BCM2708SD__SDBus__ ## x

#define TIMEOUT			        30

#define BCM2708SDUNIT_MAX               1
#define BCM2708SDCLOCK_MIN              400000

#define VCMB_PROPCHAN                   8

/* VCPOWER_* and VCCLOCK_* now live in <hardware/videocore.h>: they describe
 * the VideoCore property interface, and the USB OTG driver wants them too. */

void FNAME_BCMSDCBUS(BCMLEDCtrl)(int lvl);

UBYTE FNAME_BCMSDCBUS(BCMMMIOReadByte)(ULONG, struct sdcard_Bus *);
UWORD FNAME_BCMSDCBUS(BCMMMIOReadWord)(ULONG, struct sdcard_Bus *);
ULONG FNAME_BCMSDCBUS(BCMMMIOReadLong)(ULONG, struct sdcard_Bus *);

void FNAME_BCMSDCBUS(BCMMMIOWriteByte)(ULONG, UBYTE, struct sdcard_Bus *);
void FNAME_BCMSDCBUS(BCMMMIOWriteWord)(ULONG, UWORD, struct sdcard_Bus *);
void FNAME_BCMSDCBUS(BCMMMIOWriteLong)(ULONG, ULONG, struct sdcard_Bus *);

#endif // _SDCARDBCM2708_INTERN_H
