/*
 * Per-task CPU usage for m68k-emu68.
 *
 * TaskTag_CPUUsage (rom/task/QueryTaskTagList.c) reports iet_CpuUsage and
 * nothing else fills it here, so it has always read zero -- on this port and
 * on every other m68k, upstream's Amiga target included. Only all-pc,
 * arm-native and riscv64-opensbi ever wrote that field.
 *
 * The reason no m68k did is that the other arches timestamp every context
 * switch against a CPU register -- TSC on x86, CNTPCT on arm -- and a real
 * 68k has nothing of the sort. Under Emu68 it does: the JIT answers
 * MOVEC from control register 0x0e1 with the ARM generic counter, in two
 * AArch64 instructions and without touching the bus (Emu68
 * src/M68k_LINE4.c, stock since 2021). So the accounting the other ports do
 * is affordable here for the same reason it is affordable there.
 *
 * What this measures is wall-clock share. The JIT can also say how much
 * *guest work* a task did -- see emu68_insncount() below -- which is a
 * different and sometimes more honest number, because a task doing MMIO and
 * a task doing register arithmetic do very different amounts of m68k work
 * per second of wall clock.
 */

#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/tasks.h>
#include <proto/exec.h>

#include <etask.h>

/*
 * MOVEC from an Emu68 counter into %d0.
 *
 * The assembler has no name for these registers, so both words are spelled
 * out. 0x4e7a is MOVEC Rc,Rn; the extension word is A/D (1 bit), register
 * number (3 bits), control register (12 bits) -- so 0x00e1 reads control
 * register 0x0e1 into D0.
 *
 * Privileged, which is why these are only ever called from the dispatch path,
 * where the SR is already at supervisor level 7.
 *
 *   0x0e0  CNTFRQ     counter clock speed, in Hz
 *   0x0e1  CNTVALLO   low 32 bits of the ARM generic counter
 *   0x0e3  INSNCNTLO  low 32 bits of the m68k instruction counter
 */
#define EMU68_MOVEC_D0(creg)                                            \
    ({                                                                  \
        ULONG __v;                                                      \
        __asm__ volatile (".short 0x4e7a, " #creg "\n\t"                 \
                          "move.l %%d0,%0"                              \
                          : "=g" (__v) : : "d0");                       \
        __v;                                                            \
    })

static inline ULONG emu68_cntfrq(void)  { return EMU68_MOVEC_D0(0x00e0); }
static inline ULONG emu68_cntval(void)  { return EMU68_MOVEC_D0(0x00e1); }
static inline ULONG emu68_insncount(void) { return EMU68_MOVEC_D0(0x00e3); }

/*
 * Who has been running since the last dispatch, and from when.
 *
 * One stamp for the whole system rather than a per-task "started at": every
 * path into the dispatcher passes through the one hook below, so the interval
 * between two consecutive dispatches belongs entirely to whoever was current
 * during it. That also means no task needs its slice opened separately, which
 * is what would otherwise have to happen in three different places -- Switch,
 * m68k_VoluntarySwitch and the idle loop.
 */
static struct Task *usage_RunTask = NULL;
static ULONG        usage_RunStamp = 0;
static ULONG        usage_RunInsn = 0;

/* When the last usage window was closed, and how long a window is. */
static ULONG        usage_WindowStamp = 0;
static ULONG        usage_WindowInsn = 0;
static ULONG        usage_WindowTicks = 0;

/*
 * A window of about a second, in counter ticks.
 *
 * CNTFRQ is what the firmware programmed and is not always the rate the
 * counter actually advances at -- AROS upstream says so of the bcm2836
 * (kernel: get the arm generic-timer rate right on bcm2836), where QEMU
 * reports 19.2MHz while counting at 1MHz. It does not matter for the number
 * this file produces: usage is a ratio of two quantities in the same ticks,
 * so the rate cancels. It only sets how often the ratio is refreshed, and a
 * window that is wrong by a factor still refreshes -- just faster or slower
 * than intended. Bounded so a nonsense CNTFRQ cannot stop refreshes entirely.
 */
#define USAGE_TICKS_MIN 100000UL
#define USAGE_TICKS_MAX 100000000UL

static ULONG usage_Window(void)
{
    ULONG hz;

    if (usage_WindowTicks)
        return usage_WindowTicks;

    hz = emu68_cntfrq();

    if (hz < USAGE_TICKS_MIN || hz > USAGE_TICKS_MAX)
        hz = 1000000UL;                 /* assume 1MHz and keep refreshing */

    usage_WindowTicks = hz;
    return usage_WindowTicks;
}

/*
 * A ratio scaled to 0..0xffffffff, the range the other ports report, computed
 * without a 64-bit divide: libgcc's __udivdi3 is not something ROM-resident
 * code should be reaching for. Shifting both terms down until the numerator
 * fits keeps 16 bits of ratio, which is 0.0015% -- far finer than anything
 * this number means.
 */
static ULONG usage_Ratio(ULONG part, ULONG whole)
{
    if (!whole)
        return 0;

    if (part >= whole)
        return 0xffffffff;

    while (part > 0xffff)
    {
        part >>= 1;
        whole >>= 1;
    }

    if (!whole)
        return 0xffffffff;

    return ((part << 16) / whole) << 16;
}

static void usage_Close(struct Task *t, ULONG now, ULONG insn,
                        ULONG window, ULONG windowInsn)
{
    struct IntETask *iet;
    ULONG busy, work;

    if (!t || !(t->tc_Flags & TF_ETASK) || !t->tc_UnionETask.tc_ETask)
        return;

    iet = IntETask(t->tc_UnionETask.tc_ETask);

    busy = (ULONG)iet->iet_private2;
    work = (ULONG)iet->iet_CpuInsn;

    /*
     * The running task has not had its slice committed yet -- the hook only
     * closes a slice when the next dispatch happens -- so without this it
     * looks idle exactly when it is the busiest thing on the machine.
     */
    if (t == usage_RunTask)
    {
        busy += now - usage_RunStamp;
        work += insn - usage_RunInsn;
    }

    iet->iet_CpuUsage = usage_Ratio(busy - (ULONG)iet->iet_LastBusy, window);
    iet->iet_InsnUsage = usage_Ratio(work - (ULONG)iet->iet_LastInsn, windowInsn);

    iet->iet_LastBusy = busy;
    iet->iet_LastInsn = work;
}

static void usage_Sweep(ULONG now, ULONG insn)
{
    ULONG window = now - usage_WindowStamp;
    ULONG windowInsn = insn - usage_WindowInsn;
    struct Task *t;

    usage_WindowStamp = now;
    usage_WindowInsn = insn;

    /*
     * Walked with the SR already at level 7 -- the dispatcher masked it on the
     * way in -- so the lists cannot change underneath. Exec here is not built
     * __AROSEXEC_SMP__, so there is no second core to lock against either.
     */
    usage_Close(SysBase->ThisTask, now, insn, window, windowInsn);

    ForeachNode(&SysBase->TaskReady, t)
        usage_Close(t, now, insn, window, windowInsn);

    ForeachNode(&SysBase->TaskWait, t)
        usage_Close(t, now, insn, window, windowInsn);
}

/*
 * Called from exec/dispatch.S once the incoming task is current.
 *
 * Everything that reaches the CPU goes through there -- the involuntary
 * Switch, the Wait() path in m68k_VoluntarySwitch, and the idle loop's return
 * from emu68_DispatchFrame -- which is why one hook is enough where the arm
 * port needs its accounting in both cpu_Switch and cpu_Dispatch.
 */
void emu68_CpuUsageDispatch(void)
{
    ULONG now = emu68_cntval();
    ULONG insn = emu68_insncount();
    struct Task *task = SysBase->ThisTask;

    /*
     * 32-bit subtraction throughout. The counter wraps, and unsigned
     * arithmetic is right across the wrap for any two readings less than a
     * whole period apart -- dispatches are milliseconds apart and the window
     * is about a second, so both are. Only 0x0e1 and 0x0e3 are read and never
     * their high halves: those are the registers the harness's Musashi also
     * answers, so the same kernel runs on both backends.
     */
    if (usage_RunTask && (usage_RunTask->tc_Flags & TF_ETASK) &&
        usage_RunTask->tc_UnionETask.tc_ETask)
    {
        struct IntETask *iet = IntETask(usage_RunTask->tc_UnionETask.tc_ETask);

        iet->iet_private2 += now - usage_RunStamp;
        iet->iet_CpuInsn += insn - usage_RunInsn;
    }

    usage_RunTask = task;
    usage_RunStamp = now;
    usage_RunInsn = insn;

    if (!usage_WindowStamp)
    {
        usage_WindowStamp = now;
        usage_WindowInsn = insn;
        return;
    }

    if ((now - usage_WindowStamp) >= usage_Window())
        usage_Sweep(now, insn);
}
