/* Prepare the 68-byte task frame consumed by the Emu68 scheduler. */

#include <exec/execbase.h>
#include <utility/tagitem.h>
#include <aros/m68k/cpucontext.h>
#include <proto/kernel.h>
#include <string.h>
#include "exec_intern.h"
#include "exec_util.h"

#define _PUSH(sp, val) *--sp = (IPTR)(val)
#define EMU68_TASK_FRAME_SIZE 68

BOOL PrepareContext(struct Task *task, APTR entryPoint, APTR fallBack,
                    const struct TagItem *tagList, struct ExecBase *SysBase)
{
    IPTR args[8] = {0};
    WORD numargs = 0;
    IPTR *sp = task->tc_SPReg;
    struct ExceptionContext *ctx;
    UBYTE *frame;

    if (!(task->tc_Flags & TF_ETASK)) return FALSE;
    ctx = KrnCreateContext();
    task->tc_UnionETask.tc_ETask->et_RegFrame = ctx;
    if (!ctx) return FALSE;
    while (tagList) {
        switch (tagList->ti_Tag) {
        case TAG_MORE: tagList = (const struct TagItem *)tagList->ti_Data; continue;
        case TAG_SKIP: tagList += tagList->ti_Data; break;
        case TAG_DONE: tagList = NULL; break;
#define HANDLEARG(x) case TASKTAG_ARG ## x: args[x-1]=(IPTR)tagList->ti_Data; if (x>numargs) numargs=x; break;
        HANDLEARG(1) HANDLEARG(2) HANDLEARG(3) HANDLEARG(4)
        HANDLEARG(5) HANDLEARG(6) HANDLEARG(7) HANDLEARG(8)
        }
        if (tagList) tagList++;
    }
    while (numargs--) _PUSH(sp, args[numargs]);
    _PUSH(sp, fallBack);
    frame = (UBYTE *)sp - EMU68_TASK_FRAME_SIZE;
    memset(frame, 0, EMU68_TASK_FRAME_SIZE);
    *(IPTR *)(frame + 0) = (IPTR)entryPoint;
    *(UWORD *)(frame + 4) = 0;
    *(UWORD *)(frame + 6) = 0;
    ctx->pc = (IPTR)entryPoint;
    ctx->a[7] = (IPTR)sp;
    ctx->sr = 0;
    task->tc_SPReg = frame;
    return TRUE;
}
