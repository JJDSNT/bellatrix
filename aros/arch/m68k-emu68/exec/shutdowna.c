/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: ShutdownA() - Shut down the operating system (m68k-emu68).
*/

/*
 * Until this file existed the target had no ShutdownA() of its own, so it got
 * rom/exec/shutdowna.c: run the reset callbacks, return 0, and leave the board
 * exactly as it was. That is the honest answer for a machine with nothing to
 * ask, and this one is not that machine -- Emu68 owns the bare metal of a
 * Raspberry Pi, and the Pi has a power-management block that both resets and
 * halts it.
 *
 * The sequences below are arch/aarch64-native/kernel/syscall.c's, which drives
 * the same silicon from the other side of the same peripheral window. Nothing
 * here is a new address: the block base is the one this port already carries
 * as GPIO_PADS (ARM_PERIIOBASE + 0x100000 -- the pads registers live inside
 * the PM page, at +0x2c), and the register offsets and the 0x5a password are
 * that driver's, unchanged.
 *
 * The one thing that could not be copied is the byte order. Those ports
 * dereference these registers directly because ARM runs little-endian there;
 * Emu68 maps the peripheral block straight through to a big-endian guest, so
 * every access converts -- the same rule as platform/bcm283x/system_timer.c.
 */

#include <aros/debug.h>
#include <aros/kernel.h>
#include <aros/macros.h>

#include <exec/execbase.h>
#include <exec/pm.h>

#include <proto/exec.h>
#include <proto/kernel.h>

#include "exec_intern.h"
#include "exec_util.h"

/* Offsets into the PM block at ARM_PERIIOBASE + 0x100000. */
#define PM_BLOCK_OFFSET 0x100000
#define PM_RSTC         0x1c
#define PM_RSTS         0x20
#define PM_WDOG         0x24

#define PM_PASSWORD     0x5a000000
#define PM_RSTC_WRCFG_FULL_RESET 0x20
#define PM_RSTC_WRCFG_MASK       0x30

/*
 * How long to wait for the watchdog to take the board away.
 *
 * PM_WDOG counts in units of 1/65536 s, so the ten it is given below expires
 * in well under a millisecond. Anything past that means nothing is listening
 * -- an emulator that does not model the PM block, most likely -- and in that
 * case returning is far better than spinning forever, which is what the
 * arm-side implementations do and what would turn "cannot power off" into a
 * hang.
 */
#define PM_WAIT_SPINS 20000000UL

static ULONG pm_read(IPTR pm, ULONG reg)
{
    return AROS_LE2LONG(*(volatile ULONG *)(pm + reg));
}

static void pm_write(IPTR pm, ULONG reg, ULONG val)
{
    *(volatile ULONG *)(pm + reg) = AROS_LONG2LE(val);
}

/* Arm the watchdog for an immediate full reset. Does not return, normally. */
static void pm_full_reset(IPTR pm)
{
    ULONG rstc = pm_read(pm, PM_RSTC);
    volatile ULONG spin;

    pm_write(pm, PM_WDOG, PM_PASSWORD | 10);
    pm_write(pm, PM_RSTC,
             PM_PASSWORD | (rstc & ~PM_RSTC_WRCFG_MASK) | PM_RSTC_WRCFG_FULL_RESET);

    for (spin = 0; spin < PM_WAIT_SPINS; spin++)
        ;
}

/* See rom/exec/shutdowna.c for documentation */

AROS_LH1(ULONG, ShutdownA,
    AROS_LHA(ULONG, action, D0),
    struct ExecBase *, SysBase, 173, Exec)
{
    AROS_LIBFUNC_INIT

    IPTR pm;

    /*
     * KernelBase is not a variable to declare here: exec_intern.h spells it
     * PrivExecBase(SysBase)->KernelBase, so exec code already has it and a
     * local of that name collides with the macro rather than shadowing it.
     */
    if (!KernelBase)
        return 0;

    pm = (IPTR)KrnGetSystemAttr(KATTR_PeripheralBase) + PM_BLOCK_OFFSET;

    switch (action & SD_ACTION_MASK)
    {
    case SD_ACTION_POWEROFF:
        Exec_DoResetCallbacks((struct IntExecBase *)SysBase, action);

        /*
         * Halt through the same watchdog as a reset, with the boot partition
         * set to 63 first: the firmware reads that as "do not boot" and drops
         * the board into its low-power state instead of coming back up. The
         * partition number lives in the even bits of PM_RSTS, so 63 is 0x555.
         */
        {
            ULONG rsts = pm_read(pm, PM_RSTS);

            rsts &= ~0xfffffaaaUL;
            pm_write(pm, PM_RSTS, PM_PASSWORD | rsts | 0x555);
        }

        pm_full_reset(pm);
        break;

    case SD_ACTION_COLDREBOOT:
    case SD_ACTION_REBOOT:      /* the Pi has one hardware reboot path */
        Exec_DoResetCallbacks((struct IntExecBase *)SysBase, action);
        pm_full_reset(pm);
        break;
    }

    /*
     * Unknown action, or the hardware did not take it. Zero is what the
     * generic implementation returns and what a caller already handles;
     * reaching here at all is worth saying, because it means a shutdown the
     * user asked for did not happen.
     */
    bug("[Exec] ShutdownA: action %lu did not take effect\n",
        (unsigned long)(action & SD_ACTION_MASK));

    return 0;

    AROS_LIBFUNC_EXIT
}
