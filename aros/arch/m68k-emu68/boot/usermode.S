/*
 * Enter the AROS coldstart continuation in m68k user mode.
 */

	.text
	.balign 4
	.globl emu68_enter_user
	.type emu68_enter_user,@function

emu68_enter_user:
	move.l	4(%sp),%a0
	move.l	8(%sp),%a1
	move.l	%a1,%usp
	clr.w	-(%sp)		/* 68010+ format/vector */
	move.l	%a0,-(%sp)	/* user PC */
	clr.w	-(%sp)		/* user SR */
	rte

	.size emu68_enter_user,.-emu68_enter_user
