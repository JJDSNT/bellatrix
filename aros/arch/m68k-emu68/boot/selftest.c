/*
 * Timer and scheduler validation for the native Emu68 target.
 *
 * Off by default. This existed to answer one question -- whether the level-6
 * autovector path, timer.device on top of it, and preemptive scheduling
 * actually work -- and it has answered it: the full suite, including a
 * two-minute soak with two spinning workers, passes. Leaving it on costs
 * two minutes of every boot and holds the machine at priority 125 while it
 * runs. Set to 1 to re-run it after touching interrupt delivery, the
 * platform timer, or the scheduler.
 */
#define EMU68_SELFTEST 0

#if EMU68_SELFTEST

#include "boot.h"

#include <aros/asmcall.h>
#include <devices/timer.h>
#include <exec/errors.h>
#include <exec/nodes.h>
#include <exec/resident.h>
#include <proto/exec.h>
#include <utility/tagitem.h>

extern volatile ULONG emu68_platform_ticks;

/*
 * Rough upper bound on how long to spin waiting for one platform-timer
 * interrupt. Deliberately generous: at a VBlank-derived interval a tick is
 * tens of milliseconds away, and this only needs to be large enough not to
 * misfire while still keeping a broken timer from hanging the boot.
 */
#define EMU68_TICK_SPIN_LIMIT 100000000UL

static volatile ULONG worker_counter_a;
static volatile ULONG worker_counter_b;

/*
 * The workers exist only to give the probe something to observe being
 * scheduled. They run at a priority above the boot task, so they have to be
 * told to stop -- otherwise they starve dosboot.resource for good once the
 * probe is finished.
 */
static volatile BOOL workers_stop;

static void scheduler_worker_a(void)
{
    while (!workers_stop)
        worker_counter_a++;
}

static void scheduler_worker_b(void)
{
    while (!workers_stop)
        worker_counter_b++;
}

static struct timerequest *open_timer_request(struct MsgPort *port)
{
    struct timerequest *request;

    request = (struct timerequest *)
        CreateIORequest(port, sizeof(struct timerequest));
    if (!request)
        return NULL;

    if (OpenDevice(TIMERNAME, UNIT_MICROHZ,
                   (struct IORequest *)request, 0) != 0)
    {
        DeleteIORequest((struct IORequest *)request);
        return NULL;
    }

    return request;
}

static void close_timer_request(struct timerequest *request)
{
    if (request)
    {
        CloseDevice((struct IORequest *)request);
        DeleteIORequest((struct IORequest *)request);
    }
}

static ULONG timer_device_probe(void)
{
    struct MsgPort *port;
    struct timerequest *request;
    struct timeval before;
    ULONG result = 0;

    port = CreateMsgPort();
    if (!port)
        return 0;

    request = open_timer_request(port);
    if (request)
    {
        request->tr_node.io_Command = TR_GETSYSTIME;
        DoIO((struct IORequest *)request);
        if (request->tr_time.tv_secs != 0 ||
            request->tr_time.tv_micro != 0)
        {
            LONG elapsed_secs;
            LONG elapsed_micro;

            before = request->tr_time;
            result |= EMU68_BOOT_TIMER_TICKING;
            emu68_boot_context.flags |= EMU68_BOOT_TIMER_TICKING;
            emu68_set_stage(EMU68_STAGE_TIMER_DEVICE);
            emu68_console_puts(
                "[AROS/Emu68] timer.device clock is advancing\n");

            request->tr_node.io_Command = TR_ADDREQUEST;
            request->tr_time.tv_secs = 0;
            request->tr_time.tv_micro = 40000;
            if (DoIO((struct IORequest *)request) == 0)
            {
                result |= EMU68_BOOT_TIMER_WAKEUP;
                request->tr_node.io_Command = TR_GETSYSTIME;
                DoIO((struct IORequest *)request);
                elapsed_secs = (LONG)request->tr_time.tv_secs -
                               (LONG)before.tv_secs;
                elapsed_micro = (LONG)request->tr_time.tv_micro -
                                (LONG)before.tv_micro;
                if (elapsed_micro < 0)
                {
                    elapsed_secs--;
                    elapsed_micro += 1000000;
                }
                if (elapsed_secs > 0 ||
                    (elapsed_secs == 0 && elapsed_micro >= 40000))
                    result |= EMU68_BOOT_TIMER_COHERENT;
            }
        }
        close_timer_request(request);
    }
    DeleteMsgPort(port);
    return result;
}

static ULONG timer_device_concurrency_probe(void)
{
    struct MsgPort *port;
    struct timerequest *short_request = NULL;
    struct timerequest *long_request = NULL;
    ULONG before_a;
    ULONG before_b;
    ULONG result = 0;

    port = CreateMsgPort();
    if (!port)
        return 0;

    short_request = open_timer_request(port);
    long_request = open_timer_request(port);
    if (!short_request || !long_request)
        goto out;

    before_a = worker_counter_a;
    before_b = worker_counter_b;
    short_request->tr_node.io_Command = TR_ADDREQUEST;
    short_request->tr_time.tv_secs = 0;
    short_request->tr_time.tv_micro = 40000;
    long_request->tr_node.io_Command = TR_ADDREQUEST;
    long_request->tr_time.tv_secs = 0;
    long_request->tr_time.tv_micro = 80000;

    SendIO((struct IORequest *)long_request);
    SendIO((struct IORequest *)short_request);
    if (WaitIO((struct IORequest *)short_request) == 0 &&
        CheckIO((struct IORequest *)long_request) == NULL)
        result |= EMU68_BOOT_TIMER_CONCURRENT;
    if (WaitIO((struct IORequest *)long_request) != 0)
        goto out;
    if (worker_counter_a != before_a && worker_counter_b != before_b)
        result |= EMU68_BOOT_TIMER_PREEMPT;

    long_request->tr_node.io_Command = TR_ADDREQUEST;
    long_request->tr_time.tv_secs = 5;
    long_request->tr_time.tv_micro = 0;
    SendIO((struct IORequest *)long_request);
    if (AbortIO((struct IORequest *)long_request) == 0 &&
        WaitIO((struct IORequest *)long_request) == IOERR_ABORTED)
        result |= EMU68_BOOT_TIMER_ABORT;

out:
    close_timer_request(long_request);
    close_timer_request(short_request);
    DeleteMsgPort(port);
    return result;
}

static BOOL timer_device_soak_probe(void)
{
    struct MsgPort *port;
    struct timerequest *request;
    ULONG second;
    BOOL passed = TRUE;

    port = CreateMsgPort();
    if (!port)
        return FALSE;
    request = open_timer_request(port);
    if (!request)
    {
        DeleteMsgPort(port);
        return FALSE;
    }

    emu68_set_stage(EMU68_STAGE_TIMER_SOAK);
    emu68_console_puts("[AROS/Emu68] starting two-minute timer soak\n");
    for (second = 1; second <= 120; second++)
    {
        ULONG before_a = worker_counter_a;
        ULONG before_b = worker_counter_b;

        request->tr_node.io_Command = TR_ADDREQUEST;
        request->tr_time.tv_secs = 1;
        request->tr_time.tv_micro = 0;
        if (DoIO((struct IORequest *)request) != 0)
        {
            emu68_console_puts(
                "[AROS/Emu68] timer soak: timer request failed\n");
            passed = FALSE;
            break;
        }
        if (worker_counter_a == before_a)
        {
            emu68_console_puts(
                "[AROS/Emu68] timer soak: worker A was starved\n");
            passed = FALSE;
            break;
        }
        if (worker_counter_b == before_b)
        {
            emu68_console_puts(
                "[AROS/Emu68] timer soak: worker B was starved\n");
            passed = FALSE;
            break;
        }

        emu68_boot_context.timer_soak_seconds = second;
        if (second == 60)
        {
            emu68_set_stage(EMU68_STAGE_TIMER_MINUTE1);
            emu68_console_puts("[AROS/Emu68] timer soak: one minute\n");
        }
        else if (second == 120)
        {
            emu68_set_stage(EMU68_STAGE_TIMER_MINUTE2);
            emu68_console_puts("[AROS/Emu68] timer soak: two minutes\n");
        }
    }

    close_timer_request(request);
    DeleteMsgPort(port);
    return passed && emu68_boot_context.timer_soak_seconds == 120;
}

/*
 * Wait for three consecutive platform-timer interrupts, reporting each one
 * and giving up instead of spinning forever if delivery stalls.
 *
 * Where this stops localises a bring-up failure precisely:
 *
 *  - No tick at all: level 6 never reached AROS. There is nothing to arm on
 *    the Emu68 side -- a host interrupt is handed to the CPU as an IPL and
 *    the arbitration honours it against the SR mask, see
 *    platform/platform.c -- so the candidates are all below or above that:
 *    the ARM interrupt never reached Emu68's vector, the interrupt
 *    controller never unmasked the source, the System Timer's compare never
 *    matched, or the SR mask never dropped far enough to take a level 6.
 *
 *  - Tick 1 and then a stall: delivery works but the source is not being
 *    released. Emu68 dropped the level as it took the exception and our RTE
 *    lowers the SR mask, so nothing on the bridge needs acknowledging; what
 *    does is the peripheral itself, through the controller's Dispatch().
 */
static void platform_timer_probe(void)
{
    static const char *const tick_message[3] = {
        "[AROS/Emu68] platform timer: tick 1\n",
        "[AROS/Emu68] platform timer: tick 2\n",
        "[AROS/Emu68] platform timer: tick 3\n",
    };
    ULONG base = emu68_platform_ticks;
    ULONG seen;

    /*
     * Count from wherever the platform timer already is. This probe runs long
     * after platform_timer_start(), so an absolute comparison against zero
     * would be satisfied by ticks that arrived before we were even created
     * and would prove nothing.
     */
    for (seen = 0; seen < 3; seen++)
    {
        ULONG spins = 0;

        while (emu68_platform_ticks <= base + seen &&
               spins < EMU68_TICK_SPIN_LIMIT)
            spins++;

        if (emu68_platform_ticks <= base + seen)
        {
            emu68_console_puts(seen == 0
                ? "[AROS/Emu68] platform timer: no interrupt delivered\n"
                : "[AROS/Emu68] platform timer: interrupt delivery stalled\n");
            return;
        }

        emu68_console_puts(tick_message[seen]);
    }
}

static void scheduler_probe(void)
{
    ULONG timer_result;
    ULONG concurrency_result;

    emu68_boot_context.flags |= EMU68_BOOT_TASK_RUNNING;
    emu68_set_stage(EMU68_STAGE_TASK_RUNNING);
    emu68_console_puts("[AROS/Emu68] scheduled task is running\n");
    platform_timer_probe();

    emu68_set_stage(EMU68_STAGE_TIMER_RUNNING);
    timer_result = timer_device_probe();
    if (timer_result & EMU68_BOOT_TIMER_WAKEUP)
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_WAKEUP;
        emu68_set_stage(EMU68_STAGE_TIMER_WAKEUP);
        emu68_console_puts("[AROS/Emu68] timer.device woke the task\n");
    }
    else
        emu68_console_puts("[AROS/Emu68] timer.device request failed\n");

    if (timer_result & EMU68_BOOT_TIMER_COHERENT)
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_COHERENT;
        emu68_set_stage(EMU68_STAGE_TIMER_COHERENT);
        emu68_console_puts(
            "[AROS/Emu68] timer.device elapsed time is coherent\n");
    }
    else
        emu68_console_puts(
            "[AROS/Emu68] timer.device elapsed time is invalid\n");

    emu68_console_puts("[AROS/Emu68] virtual timer interrupts are running\n");
    concurrency_result = timer_device_concurrency_probe();
    if (concurrency_result & EMU68_BOOT_TIMER_CONCURRENT)
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_CONCURRENT;
        emu68_set_stage(EMU68_STAGE_TIMER_CONCURRENT);
        emu68_console_puts(
            "[AROS/Emu68] simultaneous timer requests completed in order\n");
    }
    else
        emu68_console_puts(
            "[AROS/Emu68] simultaneous timer request test failed\n");

    if (concurrency_result & EMU68_BOOT_TIMER_ABORT)
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_ABORT;
        emu68_set_stage(EMU68_STAGE_TIMER_ABORT);
        emu68_console_puts("[AROS/Emu68] AbortIO cancelled timer request\n");
    }
    else
        emu68_console_puts("[AROS/Emu68] AbortIO timer test failed\n");

    if (concurrency_result & EMU68_BOOT_TIMER_PREEMPT)
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_PREEMPT;

    if (timer_device_soak_probe())
    {
        emu68_boot_context.flags |= EMU68_BOOT_TIMER_SOAK;
        emu68_set_stage(EMU68_STAGE_TIMER_COMPLETE);
        emu68_console_puts(
            "[AROS/Emu68] timer/scheduler soak completed successfully\n");
    }
    else
        emu68_console_puts("[AROS/Emu68] timer/scheduler soak failed\n");

    /* Release the workers and exit, so the boot can carry on into
     * dosboot.resource with the machine in its normal state. */
    workers_stop = TRUE;
}

int emu68_scheduler_selftest_start(void)
{
    BOOL result = TRUE;

    Forbid();
    if (!NewCreateTask(TASKTAG_NAME, "Emu68 timer worker A",
                       TASKTAG_PRI, 10,
                       TASKTAG_PC, scheduler_worker_a,
                       TAG_DONE) ||
        !NewCreateTask(TASKTAG_NAME, "Emu68 timer worker B",
                       TASKTAG_PRI, 10,
                       TASKTAG_PC, scheduler_worker_b,
                       TAG_DONE))
        result = FALSE;

    if (!NewCreateTask(TASKTAG_NAME, "Emu68 scheduler probe",
                       TASKTAG_PRI, 125,
                       TASKTAG_PC, scheduler_probe,
                       TAG_DONE))
        result = FALSE;
    Permit();

    return result;
}

/*
 * The selftest is a resident, not a call from the bootstrap.
 *
 * InitCode(RTF_COLDSTART) is the last thing every AROS target does and it is
 * not expected to return -- dosboot.resource's init function ends in an
 * infinite "attempt to boot / no media / retry" loop, and on a successful
 * boot it hands over to dos.library instead. arch/aarch64-native panics with
 * "System Boot Failed!" if it ever does come back. So anything the bootstrap
 * schedules after that call is unreachable by design, not by accident.
 *
 * Being a resident puts the test inside COLDSTART instead, and rt_Pri picks
 * the exact moment. timer.device is residentpri 50 and dosboot.resource is
 * residentpri -50; -49 is therefore the last slot before dosboot, with the
 * whole system -- timer.device, graphics, intuition -- already up.
 *
 * Boot with "sysdebug=InitCode" to watch the resident list and the order
 * this actually runs in (rom/kernel/prepareexecbase.c prints the roster with
 * each module's flags, rom/exec/initcode.c narrates each InitResident).
 *
 * kernel_romtags.c finds this by scanning __aros_resident_start ..
 * __aros_resident_end for RTC_MATCHWORD, so no module wrapper or .conf is
 * needed -- but the tag has to go in .aros.romtag rather than plain .rodata.
 * See boot/emu68.ld for why: as ordinary .rodata this lands inside the span
 * dosboot's rt_EndSkip tells the scanner to jump over, and is never seen.
 */
static AROS_UFH3(void, selftest_ResidentInit,
    AROS_UFHA(struct Library *, lh, D0),
    AROS_UFHA(BPTR, segList, A0),
    AROS_UFHA(struct ExecBase *, sysBase, A6))
{
    AROS_USERFUNC_INIT

    (void)lh;
    (void)segList;
    (void)sysBase;

    if (!emu68_scheduler_selftest_start())
        emu68_console_puts("[AROS/Emu68] failed to start scheduler selftest\n");

    AROS_USERFUNC_EXIT
}

static const struct Resident emu68_selftest_romtag
    __attribute__((section(".aros.romtag"), used)) =
{
    RTC_MATCHWORD,
    (struct Resident *)&emu68_selftest_romtag,
    (APTR)(&emu68_selftest_romtag + 1),
    RTF_COLDSTART,
    41,
    NT_UNKNOWN,
    -49,
    "emu68selftest",
    "emu68 timer/scheduler selftest 41.1\r\n",
    (APTR)selftest_ResidentInit
};

#endif /* EMU68_SELFTEST */
