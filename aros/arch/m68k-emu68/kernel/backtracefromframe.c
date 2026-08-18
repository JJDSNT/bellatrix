/*
    Copyright (C) 2026, The Bellatrix Project. All rights reserved.

    Desc: KrnBacktraceFromFrame() - m68k frame-pointer stack walk.
*/

#include <aros/debug.h>
#include <aros/kernel.h>
#include <aros/libcall.h>

#include <exec/execbase.h>
#include <exec/tasks.h>
#include <kernel_base.h>

#include <proto/exec.h>
#include <proto/kernel.h>

/*
 * Why this file exists.
 *
 * rom/kernel/backtracefromframe.c is a stub: its body is `return 0;` with the
 * comment "the implementation of this function is architecture-specific".
 * aarch64, x86_64 and riscv64 each override it from arch/<cpu>-all/kernel; m68k
 * never did. So every backtrace on this port reported "0 frames" regardless of
 * how anything was compiled -- there was no walker to run.
 *
 * That was mistaken for a frame-pointer problem, because the autodoc above the
 * stub talks at length about needing -fno-omit-frame-pointer. It does need it,
 * but only once a walker exists; the missing walker comes first and costs one
 * file rather than a rebuild of the tree.
 *
 * The frame layout is what GCC emits for m68k with frame pointers enabled:
 *
 *      link.w  %a6,#-N          -> pushes the caller's %a6, then %a6 = %sp
 *      ...
 *      unlk    %a6
 *
 * which leaves, at the address in %a6:
 *
 *      0(%a6)  the caller's saved %a6
 *      4(%a6)  the return address
 *
 * Same shape as the AArch64 x29 record and the x86_64 rbp chain, so the walk is
 * the same and only the validity tests are m68k's.
 */

/*
 * A frame further up the stack is always at a higher address, and no single
 * frame is anywhere near this large. The bound stops a walk that has wandered
 * into unrelated memory from printing plausible-looking rubbish, which matters
 * here because the whole point of the caller is that the heap is already
 * corrupt.
 */
#define M68K_MAX_FRAME_SPAN 0x40000UL

AROS_LH3(ULONG, KrnBacktraceFromFrame,
        AROS_LHA(APTR, frame_in, A0),
        AROS_LHA(APTR *, out_pcs, A1),
        AROS_LHA(ULONG, max_depth, D0),
        struct KernelBase *, KernelBase, 69, Kernel)
{
    AROS_LIBFUNC_INIT

    ULONG n = 0;
    IPTR *fp = (IPTR *)frame_in;
    IPTR lower = 0, upper = 0;

    if (!fp || !out_pcs || ((IPTR)fp & 1))
        return 0;

    /*
     * Bound the walk by the current task's stack when the starting frame is
     * actually in it -- and only then. This reporter is also reached from
     * supervisor and interrupt context, where the live stack is not
     * tc_SPLower..tc_SPUpper at all; insisting on those bounds there would
     * reject every frame and reproduce the "0 frames" this file exists to fix.
     */
    if (SysBase && SysBase->ThisTask)
    {
        IPTR lo = (IPTR)SysBase->ThisTask->tc_SPLower;
        IPTR hi = (IPTR)SysBase->ThisTask->tc_SPUpper;

        if (lo && hi > lo && (IPTR)fp >= lo && (IPTR)fp < hi)
        {
            lower = lo;
            upper = hi;
        }
    }

    while (n < max_depth)
    {
        IPTR saved_fp, ret;

        if ((IPTR)fp & 1)
            break;
        if (upper && ((IPTR)fp < lower || (IPTR)fp + sizeof(IPTR) * 2 > upper))
            break;

        saved_fp = fp[0];
        ret      = fp[1];

        /* An odd return address cannot have come from a BSR/JSR on m68k. */
        if (!ret || (ret & 1))
            break;

        out_pcs[n++] = (APTR)ret;

        if (saved_fp <= (IPTR)fp)
            break;
        if (saved_fp - (IPTR)fp > M68K_MAX_FRAME_SPAN)
            break;

        fp = (IPTR *)saved_fp;
    }

    return n;

    AROS_LIBFUNC_EXIT
}
