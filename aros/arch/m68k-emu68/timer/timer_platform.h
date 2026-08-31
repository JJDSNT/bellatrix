/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Platform half of timer.device for m68k-emu68.
*/

/*
 * Until this directory existed the target had no arch timer backend at all,
 * so rom/timer built its own generic files throughout -- including
 * rom/timer/ticks.c, whose EClockUpdate() is an empty template. The clock
 * then advanced only when the heartbeat fired, in whole periods, and every
 * time source in the system had that resolution. ticks.c next to this file
 * is what fixes it; the two extra fields here are the whole of what it needs.
 *
 * tb_VBlankTime stays because timer_init.c still reports the nominal period
 * and it is the honest name for it. It is no longer what advances the clock.
 *
 * A warning for whoever adds the next field: this header is NEW, and it
 * shadows rom/timer/timer_platform.h from earlier on the include path.
 * ccache's direct mode gets that wrong -- its manifest lists the header this
 * one displaced, finds it unchanged, and serves an object compiled against
 * it. sizeof(struct TimerBase) then differs between translation units, the
 * romtag sizes the library base from the smaller one, and the first write
 * past that point lands in the next heap block's header. It surfaces as a
 * TLSF free-list corruption inside timer.device's init, nowhere near the
 * cause, and deleting the object does not help because the same false hit is
 * served again. See ISSUE-0085.
 */
struct PlatformTimer
{
    LONG            tb_TimerIRQNum;     /* Timer IRQ number                  */
    struct timeval  tb_VBlankTime;      /* Nominal heartbeat interval        */
    IPTR            tbp_periiobase;     /* Where Emu68 mapped the peripherals */
    ULONG           tbp_CLO;            /* System-timer counter last consumed */
};

/*
 * SYSTIMER_* are written against ARM_PERIIOBASE. Emu68 maps the Pi
 * peripherals where it likes, so the base is discovered at run time and
 * carried in the library base -- the same arrangement arch/aarch64-raspi
 * uses. Expanded only where a SYSTIMER_* register is named, which is ticks.c
 * and timer_init.c; the generic timer sources include this header through
 * timer_intern.h and never touch one.
 */
#define ARM_PERIIOBASE TimerBase->tb_Platform.tbp_periiobase
#include <hardware/bcm2708.h>
