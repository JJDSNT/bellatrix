/* Restore an Emu68 task context including the format/vector word. */

#include "aros/m68k/asm.h"

        .text
        .balign 4
        .globl  AROS_SLIB_ENTRY(Dispatch,Exec,10)
        .globl  __Dispatch_this

        .func AROS_SLIB_ENTRY(Dispatch,Exec,10)
AROS_SLIB_ENTRY(Dispatch,Exec,10):
__Dispatch_this:
        move.l  4,%a6
        ori.w   #0x0700,%sr
        lea.l   TaskReady(%a6),%a0
        move.l  %a0@,%a3
        move.l  %a3@,%d0
        beq.s   1f
        cmp.b   #TS_READY,tc_State(%a3)
        bne.s   1f
        move.b  tc_Flags(%a3),%d1
        and.b   #TF_DISPATCH_SPECIAL,%d1
        bne.s   1f
        move.w  AttnFlags(%a6),%d1
        and.w   #AFF_FPU,%d1
        bne.s   1f
        move.l  %d0,%a0@
        move.l  %d0,%a1
        move.l  %a0,ln_Pred(%a1)
        addq.l  #1,DispCount(%a6)
        move.l  %a3,ThisTask(%a6)
        move.w  Quantum(%a6),Elapsed(%a6)
        and.w   #SFF_QuantumClear,SysFlags(%a6)
        move.b  #TS_RUN,tc_State(%a3)
        move.b  tc_IDNestCnt(%a3),IDNestCnt(%a6)
        move.l  tc_SPReg(%a3),%a5
        bra.s   2f
1:
        jsr     emu68_DispatchFrame
        move.l  %d0,%a5
2:
        /*
         * Per-task CPU accounting (../kernel/cpuusage.c). Placed here, after
         * ThisTask is current and before the frame is restored, because every
         * way onto the CPU passes through this label -- the involuntary
         * Switch, the Wait() path in m68k_VoluntarySwitch, and the idle loop's
         * return from emu68_DispatchFrame. One hook where the arm port needs
         * its accounting in both cpu_Switch and cpu_Dispatch.
         *
         * Safe to call here: %a5 holds the frame and is callee-saved, and
         * everything the jsr may clobber (%d0-%d1/%a0-%a1) is reloaded from
         * the frame by the movem below. The SR is already at level 7, which
         * the MOVEC it issues requires.
         */
        jsr     emu68_CpuUsageDispatch
        /*
         * No Paula write here. Host interrupts arrive as a level in INTF.IPL
         * (patches/emu68/0003) and the SR mask restored by the rte below is
         * what gates them -- writing INTENA would be a page fault per dispatch
         * for a register nothing reads.
         */
        lea.l   %a5@(68),%a2
        move.l  %a2,%usp
        move.w  %a5@(6),%sp@-
        move.l  %a5@,%sp@-
        move.w  %a5@(4),%sp@-
        movem.l %a5@(8),%d0-%d7/%a0-%a6
        rte
        .endfunc
