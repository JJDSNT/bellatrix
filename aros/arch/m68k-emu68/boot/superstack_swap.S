/*
    Desc: SuperstackSwap() - move the supervisor stack onto allocated memory

    Must be called through Exec/Supervisor(), from user mode.

    Emu68 hands the guest a supervisor stack of its own choosing at entry
    (see boot/entry.S) and AROS keeps running on it. arch/m68k-amiga does not:
    boot/start.c allocates SS_STACK_SIZE and swaps onto it in doInitCode(),
    before InitCode(RTF_COLDSTART), so that every task that later traps into
    supervisor is using a stack Exec owns and sized. This is our equivalent,
    and it is the same code as arch/m68k-amiga/boot/superstack_swap.S -- the
    sequence is dictated by the m68k exception frame, not by the machine.

    On entry the stack holds the frame Exec_Supervisor_Trap left for us:

        UWORD 0x0020    (68010+ only -- the format/vector word)
        ULONG PC
        UWORD SR

    A6 = SysBase, A5 = this function. D0/D1 and A0/A1 are scratch.

    The frame has to be copied to the top of the new stack before the RTE,
    because RTE pops it from wherever SP points at that moment.
*/

	#include "aros/m68k/asm.h"

	.text
	.balign 4
	.globl SuperstackSwap
SuperstackSwap:
	move.l	%a6@(SysStkUpper),%a0
	moveq.l	#-6,%d0
	btst	#0, %a6@(AttnFlags+1)	// 68010+: the frame carries a format word
	beq	0f
	subq.l	#2,%d0
0:
	lea.l	%a0@(%d0),%a1		// where the copied frame will start
1:
	move.w	%sp@+,%a0@(%d0)
	addq.l	#2,%d0
	bne	1b
	move.l	%a1,%sp
	rte
