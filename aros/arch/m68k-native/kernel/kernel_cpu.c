/*
 * cpu_Exception, and deliberately nothing else.
 *
 * arch/m68k-all/kernel/kernel_cpu.c carries three things: this trampoline,
 * and m68k_SwitchTail/m68k_DispatchFrame -- the 66-byte scheduler policy. This
 * port replaces the scheduler in rom/exec (see ../exec/), so those two have no
 * caller here, but the file belongs to the kernel module and the exec-side
 * override does not reach it: they were being linked into every image as dead
 * code carrying the wrong frame size.
 *
 * That is not merely wasteful. Dead code that looks live is a trap -- reading
 * `for (i = 0; i < 66; i++)` in a linked object sent one investigation down a
 * blind alley on 2026-08-06. Overriding the file removes both.
 *
 * cpu_Exception itself is live: ../kernel/context.c installs it as the return
 * address of a shifted frame when a task has TF_EXCEPT set.
 */

#include <exec/execbase.h>
#include <proto/exec.h>

asm (
        "       .text\n"
        "       .align 4\n"
        "       .globl cpu_Exception\n"
        "cpu_Exception:\n"
        "       movem.l %d0-%d1/%a0-%a1/%a6,%sp@-\n"
        "       move.l  (4),%a6\n"
        "       jsr     %a6@(-1 * 6 * 11 /* Exception */)\n"
        "       movem.l %sp@+,%d0-%d1/%a0-%a1/%a6\n"
        "       rts\n"
);
