/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: On-demand EClock read for the BCM283x free-running system timer.
*/

#include <aros/macros.h>

#include <proto/exec.h>

#include "timer_intern.h"
#include "timer_macros.h"

/*
 * Bring the clock up to date from the hardware.
 *
 * timer.device calls this under Disable() at the top of GetSysTime(),
 * GetUpTime(), ReadEClock() and every TR_ADDREQUEST -- precisely so the
 * reading is current. With rom/timer's empty template in its place the clock
 * only moved when the heartbeat fired, so every time source in the system had
 * that resolution: 20ms at the 50Hz this port runs its heartbeat at.
 *
 * timer_init.c's tick handler calls this as well. Both paths advance
 * tbp_CLO past what they consumed, so no interval is ever counted twice.
 */
void EClockUpdate(struct TimerBase *TimerBase)
{
    ULONG now, delta;
    struct timeval tv;

    /*
     * BCM283x registers are little-endian, and Emu68 maps the peripheral
     * block straight through without swapping -- see the note above
     * systimer_read() in platform/bcm283x/system_timer.c.
     * arch/aarch64-raspi/timer/ticks.c dereferences this register raw, which
     * is right there only because that CPU is little-endian. Copied verbatim
     * the counter would arrive byte-reversed and the clock would lurch by
     * minutes at a time instead of standing still -- a louder failure than
     * the one being fixed, but a failure the same way.
     */
    now = AROS_LE2LONG(*(volatile ULONG *)(SYSTIMER_CLO));

    /*
     * 32-bit throughout, and CHI is deliberately not read.
     *
     * The counter runs at 1MHz, so unsigned subtraction is correct across its
     * ~71 minute wrap for any two readings less than that apart. The
     * heartbeat calls this 50 times a second, and every timer.device entry
     * point calls it too, so the interval between consecutive readings is
     * milliseconds. It stops being sound only if nothing asks the time for
     * over an hour, which on this port means the heartbeat has stopped -- and
     * a stopped heartbeat is already the larger problem.
     *
     * The alternative is CHI and 64-bit arithmetic, which on m68k means a
     * libgcc division call in ROM-resident code for no gain here.
     */
    delta = now - TimerBase->tb_Platform.tbp_CLO;
    if (!delta)
        return;

    TimerBase->tb_Platform.tbp_CLO = now;

    /* Counter ticks are microseconds. */
    tv.tv_secs  = delta / 1000000;
    tv.tv_micro = delta % 1000000;

    ADDTIME(&TimerBase->tb_CurrentTime, &tv);
    ADDTIME(&TimerBase->tb_Elapsed, &tv);
}

void EClockSet(struct TimerBase *TimerBase)
{
    /*
     * Nothing to program: the counter is read-only and free-running.
     * SetSysTime()'s value lives in tb_CurrentTime, and the next
     * EClockUpdate() carries on from it.
     */
}
