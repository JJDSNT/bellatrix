/* Save an Emu68 task context including the 68010 format/vector word. */

#include "aros/m68k/asm.h"

        .text
        .balign 4
        .globl  AROS_SLIB_ENTRY(Switch,Exec,9)
        .globl  m68k_VoluntarySwitch

        .macro EMU68_SAVE_FRAME
        move.w  #0x2000,%sr
        move.l  %a5,%sp@-
        move.l  %usp,%a5
        move.l  %a6,%a5@(-4)
        move.l  %sp@+,%a5@(-8)
        lea.l   %a5@(-60),%a5
        movem.l %d0-%d7/%a0-%a4,%a5@
        lea.l   %a5@(-8),%a5
        move.l  %sp@(2),%a5@
        move.w  %sp@,%a5@(4)
        move.w  %sp@(6),%a5@(6)
        lea.l   %sp@(8),%sp

        /*
         * The frame this saved is assumed to be eight bytes -- a 68010
         * format-0 frame. Emu68 also generates format 2, which is twelve, for
         * VECTOR_DIVIDE_BY_ZERO, VECTOR_CHK and VECTOR_TRAPcc
         * (src/M68k_Exception.c), and its RTE accepts both. Saving eight bytes
         * of a twelve-byte frame while keeping the format word that says
         * "twelve" makes the restore push eight and the RTE pop twelve.
         *
         * Report rather than handle: whether such a frame ever reaches a task
         * switch on this port is a question, not an assumption. D0 is already
         * in the frame at this point, so clobbering it is free.
         */
        move.w  %a5@(6),%d0
        and.w   #0xf000,%d0
        beq.s   9f
        move.l  %a5,%sp@-
        jsr     emu68_ReportFrameFormat
        addq.l  #4,%sp
9:
        .endm

        .func AROS_SLIB_ENTRY(Switch,Exec,9)
AROS_SLIB_ENTRY(Switch,Exec,9):
        EMU68_SAVE_FRAME
        move.l  %a5,%sp@-
        jsr     emu68_SwitchTail
        addq.l  #4,%sp
        jmp     __Dispatch_this
        .endfunc

        .func m68k_VoluntarySwitch
m68k_VoluntarySwitch:
        EMU68_SAVE_FRAME

        /* No Paula write -- see the note in dispatch.S. */
        move.l  ThisTask(%a6),%a0
        move.b  %d3,%d1
        addq.b  #1,%d1
        move.b  %d1,IDNestCnt(%a6)
        move.l  tc_SigRecvd(%a0),%d0
        and.l   tc_SigWait(%a0),%d0
        bne.s   1f

        move.b  #-1,TDNestCnt(%a6)
        move.b  #4,tc_State(%a0)
        lea.l   TaskWait(%a6),%a1
        move.l  lh_TailPred(%a1),%a2
        lea.l   ln_Pred(%a1),%a3
        move.l  %a3,ln_Succ(%a0)
        move.l  %a2,ln_Pred(%a0)
        move.l  %a0,ln_Succ(%a2)
        move.l  %a0,lh_TailPred(%a1)
        move.l  %a5,tc_SPReg(%a0)
        move.b  %d1,tc_IDNestCnt(%a0)
        move.b  #-1,IDNestCnt(%a6)
        jmp     __Dispatch_this

1:
        move.w  %a5@(6),%sp@-
        move.l  %a5@,%sp@-
        move.w  %a5@(4),%sp@-
        movem.l %a5@(8),%d0-%d7/%a0-%a6
        rte
        .endfunc
