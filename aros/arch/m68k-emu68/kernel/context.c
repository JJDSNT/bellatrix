/*
 * Emu68 policy for 68-byte persistent task frames.
 *
 * Emu68 pushes an eight-byte format-0 exception frame (ExecutionLoop.c) and its
 * RTE consumes eight. The m68k-all scheduler builds six and relies on the
 * format word left over on the supervisor stack by the matching entry, which
 * balances only while entry and exit are paired. This backend carries the
 * format word in the persistent frame instead: 68 bytes, self-contained, eight
 * pushed and eight popped.
 *
 * emu68_CheckTaskFrame() is deliberately kept and deliberately silent. It costs
 * a handful of instructions per switch and says nothing unless a frame is
 * actually malformed -- which, if the 66-byte scheme is what corrupts pointers,
 * is exactly the evidence this backend exists to collect.
 */

#include <exec/execbase.h>
#include <exec/alerts.h>
#include <proto/exec.h>
#include <defines/kernel.h>
#include <kernel_base.h>
#include <kernel_debug.h>
#include <kernel_scheduler.h>

#define EMU68_TASK_FRAME_SIZE 68
extern void cpu_Exception(void);

static void emu68_CheckTaskFrame(const char *where, struct Task *task,
                                 const UBYTE *frame)
{
    ULONG pc;
    ULONG a6;
    UWORD sr;
    UWORD format;

    if (!task || !frame)
    {
        bug("[EMU68-FRAME] %s null task/frame task=%p frame=%p\n",
            where, task, frame);
        return;
    }

    pc = *(const ULONG *)(frame + 0);
    sr = *(const UWORD *)(frame + 4);
    format = *(const UWORD *)(frame + 6);
    a6 = *(const ULONG *)(frame + 64);

    if (frame < (const UBYTE *)task->tc_SPLower ||
        frame + EMU68_TASK_FRAME_SIZE > (const UBYTE *)task->tc_SPUpper ||
        (pc & 1) || (format & 0xf000))
        bug("[EMU68-FRAME] BAD %s task=%p '%s' frame=%p stack=%p..%p "
            "pc=%08lx sr=%04x fmt=%04x a6=%08lx flags=%02x state=%u\n",
            where, task,
            task->tc_Node.ln_Name ? task->tc_Node.ln_Name : "<unnamed>",
            frame, task->tc_SPLower, task->tc_SPUpper, pc, sr, format, a6,
            task->tc_Flags, task->tc_State);
}

void emu68_SwitchTail(APTR frame)
{
    struct Task *task = SysBase->ThisTask;
    struct AROSCPUContext *ctx = task->tc_UnionETask.tc_ETask->et_RegFrame;
    emu68_CheckTaskFrame("save", task, frame);
    if (SysBase->AttnFlags & AFF_FPU)
        AROS_UFC2NR(void, FpuSaveContext, AROS_UFCA(struct FpuContext *, &ctx->fpu, A0), AROS_UFCA(UWORD, (SysBase->AttnFlags & AFF_68060) ? 2 : 0, D0));
    if (SysBase->AttnFlags & AFF_68080)
        AROS_UFC1NR(void, AMMXSaveContext, AROS_UFCA(struct AMMXContext *, &ctx->ammx, A0));
    task->tc_SPReg = frame;
    core_Switch();
}

APTR emu68_DispatchFrame(void)
{
    struct Task *task;
    struct AROSCPUContext *ctx;
    UBYTE *frame;
    for (;;) {
        asm volatile ("ori #0x0700, %sr\n");
        task = core_Dispatch();
        if (task) break;
        if (SysBase->IDNestCnt >= 0) SysBase->IDNestCnt = -1;
        asm volatile ("stop #0x2000\n");
    }
    ctx = task->tc_UnionETask.tc_ETask->et_RegFrame;
    if (SysBase->AttnFlags & AFF_FPU)
        AROS_UFC2NR(void, FpuRestoreContext, AROS_UFCA(struct FpuContext *, &ctx->fpu, A0), AROS_UFCA(UWORD, (SysBase->AttnFlags & AFF_68060) ? 2 : 0, D0));
    if (SysBase->AttnFlags & AFF_68080)
        AROS_UFC1NR(void, AMMXRestoreContext, AROS_UFCA(struct AMMXContext *, &ctx->ammx, A0));
    frame = task->tc_SPReg;
    emu68_CheckTaskFrame("restore", task, frame);
    if (task->tc_Flags & TF_EXCEPT) {
        ULONG originalPC = *(ULONG *)frame;
        UBYTE *exceptionFrame = frame - sizeof(ULONG);
        unsigned int i;
        Disable();
        if (exceptionFrame <= (UBYTE *)task->tc_SPLower) Alert(AT_DeadEnd | AN_StackProbe);
        for (i = 0; i < EMU68_TASK_FRAME_SIZE; i++) exceptionFrame[i] = frame[i];
        *(ULONG *)(exceptionFrame + EMU68_TASK_FRAME_SIZE) = originalPC;
        *(ULONG *)exceptionFrame = (ULONG)cpu_Exception;
        task->tc_SPReg = exceptionFrame;
        frame = exceptionFrame;
    }
    return frame;
}
