/*
    Copyright (C) 1995-2026, The AROS Development Team. All rights reserved.

    Desc: Timer startup for m68k-emu68.

    Derived from rom/timer/timer_init.c, which this replaces for this target.
    One thing differs, and it is the reason the file exists: the tick reads
    the hardware counter instead of adding a fixed period to a software one.
    See ticks.c.
*/

/****************************************************************************************/

#include <aros/debug.h>

#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/devices.h>
#include <exec/alerts.h>
#include <exec/initializers.h>
#include <devices/timer.h>
#include <hardware/intbits.h>

#include <aros/kernel.h>

#include <proto/exec.h>
#include <proto/execlock.h>
#include <proto/kernel.h>
#include <proto/timer.h>

#include <aros/macros.h>
#include <aros/symbolsets.h>

#include LC_LIBDEFS_FILE

#include "timer_intern.h"
#include "timer_macros.h"

/*
 * The heartbeat interrupt.
 *
 * platform/bcm283x/system_timer.c owns a system-timer compare channel and
 * causes INTB_VERTB from it, so this is an ordinary interrupt server, exactly
 * as in the generic file. What is not the same is the first thing it does:
 * the generic handler advances the clock by tb_VBlankTime, a constant, which
 * makes the tick the only thing that moves time and its period the resolution
 * of every clock in the system. Here the counter is the clock, and the tick
 * merely notices how far it has run.
 */
static AROS_INTH1(VBlankInt, struct TimerBase *, TimerBase)
{
    AROS_INTFUNC_INIT

    D(bug("%s()\n", __func__);)

    /* ticks.c owns the counter arithmetic. */
    EClockUpdate(TimerBase);
    TimerBase->tb_ticks_total++;

    /*
     * Now go to handle requests.
     * We are called at rather low rate, so don't bother and process both units.
     */
    handleMicroHZ(TimerBase, SysBase);
    handleVBlank(TimerBase, SysBase);

    return 0;

    AROS_INTFUNC_EXIT
}

/****************************************************************************************/

/*
 * LIBBASE is spelled TimerBase (timer_libdefs.h), so the parameter already
 * carries the name timer_platform.h's ARM_PERIIOBASE expands to. Declaring a
 * local of that name here shadows nothing -- it collides.
 */
static int GM_UNIQUENAME(Init)(LIBBASETYPEPTR LIBBASE)
{
    struct Interrupt *is;
    APTR KernelBase;

    D(bug("%s()\n", __func__);)

#if defined(__AROSEXEC_SMP__)
    struct ExecLockBase *ExecLockBase;
    if ((ExecLockBase = OpenResource("execlock.resource")) != NULL)
    {
        LIBBASE->tb_ExecLockBase = ExecLockBase;
        LIBBASE->tb_ListLock = AllocLock();
    }
#endif

    /*
     * Opened here rather than taken from tb_KernelBase, which common_init.c
     * fills from its own ADD2INITLIB entry: nothing in this file should
     * depend on which of the two set members runs first.
     */
    KernelBase = OpenResource("kernel.resource");
    if (!KernelBase)
        return FALSE;

    /*
     * Emu68 maps the Pi peripherals where it likes, so the window is
     * discovered, never a compile-time constant -- the same value
     * platform.c found in /soc's "ranges". SYSTIMER_* in timer_platform.h
     * reads it back out of here.
     */
    LIBBASE->tb_Platform.tbp_periiobase = KrnGetSystemAttr(KATTR_PeripheralBase);

    /* If no frequency is set, assume 50Hz */
    if (SysBase->VBlankFrequency == 0)
        SysBase->VBlankFrequency = 50;

    /*
     * Here we do no checks, we simply assume we have working VBlank interrupt,
     * from whatever source it is.
     */

    LIBBASE->tb_eclock_rate = SysBase->VBlankFrequency;
    D(bug("[timer] Timer IRQ is %d, frequency is %u Hz\n", INTB_VERTB, LIBBASE->tb_eclock_rate));

    /*
     * The nominal period. It is reported and it sizes nothing else: the clock
     * is advanced from the counter, not from this.
     */
    LIBBASE->tb_Platform.tb_VBlankTime.tv_secs  = 0;
    LIBBASE->tb_Platform.tb_VBlankTime.tv_micro = 1000000 / LIBBASE->tb_eclock_rate;

    D(bug("Timer period: %ld secs, %ld micros\n",
        LIBBASE->tb_Platform.tb_VBlankTime.tv_secs, LIBBASE->tb_Platform.tb_VBlankTime.tv_micro));

    /*
     * Take the counter's starting point before anything can ask the time, so
     * the first EClockUpdate() reports the interval since here and not since
     * the machine was switched on.
     */
    LIBBASE->tb_Platform.tbp_CLO = AROS_LE2LONG(*(volatile ULONG *)(SYSTIMER_CLO));

    /* Start up the interrupt server */
    is = AllocMem(sizeof(struct Interrupt), MEMF_PUBLIC);
    if (is)
    {
        is->is_Node.ln_Pri = 0;
        is->is_Node.ln_Type = NT_INTERRUPT;
        is->is_Node.ln_Name = (STRPTR)MOD_NAME_STRING;
        is->is_Code = (VOID_FUNC)VBlankInt;
        is->is_Data = LIBBASE;

        AddIntServer(INTB_VERTB, is);
        LIBBASE->tb_TimerIRQHandle = is;

        return TRUE;
    }

    return FALSE;
}

/****************************************************************************************/

ADD2INITLIB(GM_UNIQUENAME(Init), 0)
