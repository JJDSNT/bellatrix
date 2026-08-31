/*
 * Temporary bring-up instrumentation: report the CPU exception that stops the
 * boot, instead of letting it fall into AROS's alert path and go quiet.
 *
 * Set EMU68_TRAP_PROBE to 0 -- and this whole file compiles away -- once the
 * port has a working alert path of its own. The DEBUG build of
 * arch/m68k-all/kernel/m68k_exception.c would print the same thing, but its
 * #if DEBUG block calls Exec_MagicResetCode(), which only
 * arch/m68k-amiga/exec/coldreboot.c defines, so it does not link here.
 */

#define EMU68_TRAP_PROBE 1

#if EMU68_TRAP_PROBE

#include <exec/types.h>
#include <exec/execbase.h>

#include "m68k_exception.h"

extern void emu68_console_puts(const char *text);
extern int emu68_console_putc(int chr);

static void puthex(ULONG value)
{
    static const char digits[] = "0123456789abcdef";
    char buffer[9];
    int i;

    for (i = 7; i >= 0; i--, value >>= 4)
        buffer[i] = digits[value & 15];
    buffer[8] = 0;

    emu68_console_puts(buffer);
}

/* A byte as a character, for values that turn out to be text rather than
 * addresses -- which is how "Work" was recognised. */
static int printable(int c)
{
    return (c >= 0x20 && c < 0x7f) ? c : '.';
}

static void putreg(const char *name, ULONG value)
{
    emu68_console_puts(name);
    puthex(value);
    emu68_console_puts("\n");
}

extern UWORD __aros_resident_start[];
extern UWORD __aros_resident_end[];

/*
 * Walk the stack the exception came off and print what is on it.
 *
 * The PC in the frame names the victim, not the culprit. When a wild pointer
 * is executed the reported PC is wherever control ended up -- an odd, absurd
 * address that resolves to nothing -- and every fatal exception this port has
 * produced looks like that. What identifies the path there are the return
 * addresses still on the stack, which the same trick in patches/emu68/0004
 * already uses to attribute open-bus accesses: "the longword at the top of the
 * stack is the return address of a leaf call -- which is what identifies the
 * caller passing the bad pointer, where m68kPC only names the victim".
 *
 * Printed raw rather than filtered, with a marker on the ones inside the
 * kernel image. Return addresses into modules loaded from disk live in the
 * heap and are indistinguishable from data by inspection, so deciding here
 * which longwords are "real" would throw away the ones that matter -- the
 * `ret` value seen in the open-bus reports was a heap address.
 *
 * Guarded, because a stack pointer taken from a machine that has already lost
 * control is not to be trusted: reading through a wild one would fault inside
 * the fault report.
 */
/*
 * Ask whether A6 was ever a library base.
 *
 * Every fatal exception on this port is a `jsr -LVO(A6)` -- the offsets are
 * exact multiples of six -- with an A6 that cannot be a base: 0x83e70000 and
 * 0xc1d00000 are nowhere in the heap, and 0x010012ff is odd, which a base
 * never is. That says the base register is wrong, but not whether it is a
 * pointer to something that *used* to be a library.
 *
 * A live base has a struct Library at it: an LN_Type of NT_LIBRARY (9), a
 * plausible ln_Name, and a lib_NegSize that matches the jump table below it.
 * Debris from a freed one usually keeps some of that and loses the rest, and
 * an address that was never a library has none of it. Printing the header
 * separates the three without needing a theory first.
 */
static void describe_base(const char *what, ULONG base)
{
    const struct Library *lib = (const struct Library *)base;
    const char *name;

    emu68_console_puts(what);
    puthex(base);

    if ((base & 1) || base < 0x1000 || base >= 0x34000000)
    {
        emu68_console_puts("  (not a library base)\n");

        /*
         * Then say what the value *is* instead.
         *
         * A6 has held NULL, unrelated debris, and -- decisively -- the ASCII
         * bytes "Work", which are the first four of "Workbench". That is not a
         * pointer damaged by a stray write; it is what a freed structure looks
         * like once its memory has been handed to something else. Printing the
         * four bytes as characters is what turned that from a theory into a
         * reading, so do it always rather than by hand afterwards.
         */
        emu68_console_puts("    as bytes: '");
        emu68_console_putc(printable((base >> 24) & 0xff));
        emu68_console_putc(printable((base >> 16) & 0xff));
        emu68_console_putc(printable((base >> 8) & 0xff));
        emu68_console_putc(printable(base & 0xff));
        emu68_console_puts("'\n");
        return;
    }

    emu68_console_puts("\n    ln_Type 0x");
    puthex(lib->lib_Node.ln_Type);
    emu68_console_puts(" (NT_LIBRARY is 0x9)  lib_NegSize 0x");
    puthex(lib->lib_NegSize);
    emu68_console_puts("  lib_PosSize 0x");
    puthex(lib->lib_PosSize);
    emu68_console_puts("\n    lib_OpenCnt 0x");
    puthex(lib->lib_OpenCnt);
    emu68_console_puts("  lib_Flags 0x");
    puthex(lib->lib_Flags);
    emu68_console_puts("  ln_Name 0x");
    name = lib->lib_Node.ln_Name;
    puthex((ULONG)name);

    if (name && (ULONG)name >= 0x1000 &&
        (ULONG)name < 0x34000000)
    {
        emu68_console_puts(" '");
        emu68_console_puts(name);
        emu68_console_puts("'");
    }

    emu68_console_puts("\n");
}

/*
 * How deep to go, and why the two stacks get different budgets.
 *
 * The user stack is where the path is. A crash inside Intuition's input
 * handler gave two names in the first twenty-four longwords and the rest of
 * the call chain was below them, unreported. The supervisor stack, on the
 * same occasion, spent all twenty-four on Supervisor()'s frames -- and those
 * grow *upward* from SSP into stack already released, so most of what is
 * printed there is history. It was read as a live exception loop twice before
 * anyone read supervisor.S.
 *
 * So: the user stack gets the depth, the supervisor stack gets enough to show
 * the frame that is actually live and little more.
 */
enum { TRAP_STACK_WORDS_USER = 64, TRAP_STACK_WORDS_SUPER = 12 };

static void dump_stack(const char *what, ULONG sp, int words)
{
    const ULONG *p = (const ULONG *)sp;
    int i;

    emu68_console_puts(what);
    puthex(sp);

    if ((sp & 1) || sp < 0x1000 || sp >= 0x34000000)
    {
        emu68_console_puts("  (not walkable)\n");
        return;
    }

    emu68_console_puts("\n");

    for (i = 0; i < words; i++)
    {
        ULONG v = p[i];

        emu68_console_puts("    +0x");
        puthex((ULONG)(i * 4));
        emu68_console_puts(" 0x");
        puthex(v);

        /*
         * Two markers, because there are two places a return address can
         * live and only one of them is the kernel image. A module loaded
         * from disk returns into the heap, and the comment above is right
         * that a heap longword cannot be told from data by inspection -- but
         * an *even* one in the heap is at least a candidate, and saying so
         * costs nothing and narrows the reading by hand that follows.
         */
        if (v >= (ULONG)__aros_resident_start && v < (ULONG)__aros_resident_end)
            emu68_console_puts("  <- kernel");
        else if ((v & 1) == 0 && v >= 0x02000000 && v < 0x30600000)
            emu68_console_puts("  <- heap");

        emu68_console_puts("\n");
    }
}

/*
 * Called from the stub below with a pointer to everything it saved:
 *
 *     regs[0]      USP
 *     regs[1..8]   D0-D7
 *     regs[9..15]  A0-A6
 *     byte 64      SR      (UWORD)
 *     byte 66      PC      (ULONG)
 *     byte 70      format  (UWORD, low 12 bits are the vector offset)
 *
 * Emu68 prints its own register dump when it takes the fault itself, but with
 * the vectors populated the guest handles the exception and Emu68 never sees
 * one -- so this has to carry the same information.
 *
 * Does not return: the point is to freeze the machine with the answer on the
 * serial line rather than let a second fault overwrite it.
 */
void emu68_trap_report(ULONG *regs)
{
    /*
     * Report the first trap and nothing else.
     *
     * This function never returns -- it ends in a halt loop -- so a trap taken
     * while it is running does not unwind, it re-enters. Everything below then
     * runs again on a machine that is already broken: it walks two stacks and
     * follows A6 as a library base, and any one of those reads can be the
     * fault that brings it back here. Emu68 sees the result and says so:
     *
     *     [JIT:SYS] RE-ENTERED at depth 1 on core 0
     *     [JIT:SYS] runaway exception recursion, halting core 0
     *
     * and the core is halted with none of the first trap's registers printed.
     * That is the worst possible outcome for a reporter: the crash that
     * mattered is replaced by the crash the reporter caused.
     *
     * So the second entry says one line and stops. Observed chasing gears
     * under ISSUE-0045, where the whole report was lost this way.
     */
    static int reporting;

    if (reporting)
    {
        emu68_console_puts("[AROS/Emu68] trap while reporting a trap"
                           " -- stopping here\n");
        for (;;)
            ;
    }
    reporting = 1;

    {
    static const char *const dnames[] = {
        "  D0 0x", "  D1 0x", "  D2 0x", "  D3 0x",
        "  D4 0x", "  D5 0x", "  D6 0x", "  D7 0x"
    };
    static const char *const anames[] = {
        "  A0 0x", "  A1 0x", "  A2 0x", "  A3 0x",
        "  A4 0x", "  A5 0x", "  A6 0x"
    };
    const UWORD *frame = (const UWORD *)&regs[16];
    ULONG pc = ((ULONG)frame[1] << 16) | frame[2];
    int i;

    emu68_console_puts("[AROS/Emu68] CPU exception vector 0x");
    puthex(frame[3] & 0x0fff);
    emu68_console_puts(" at PC 0x");
    puthex(pc);
    emu68_console_puts("\n  SR 0x");
    puthex(frame[0]);
    emu68_console_puts("\n");

    for (i = 0; i < 8; i++)
        putreg(dnames[i], regs[1 + i]);
    for (i = 0; i < 7; i++)
        putreg(anames[i], regs[9 + i]);
    putreg(" USP 0x", regs[0]);

    /*
     * The supervisor stack continues immediately above the exception frame --
     * SR, PC and the format word are 8 bytes -- so that is where a call chain
     * taken in supervisor mode is. If the exception happened in user mode
     * (SR bit 13 clear) the chain is on the user stack instead, so print both
     * rather than choose: one of them is the interesting one and the other
     * costs a few lines.
     */
    describe_base(" A6 as a library base: ", regs[15]);

    dump_stack("  SSP 0x", (ULONG)&regs[16] + 8, TRAP_STACK_WORDS_SUPER);
    dump_stack("  USP 0x", regs[0], TRAP_STACK_WORDS_USER);
    }

    for (;;)
        ;
}

/*
 * tc_TrapCode calling convention, as arch/m68k-amiga/kernel/amiga_irq.c's
 * DECLARE_TrapCode() uses it: the exception Id is pushed above the frame and
 * has to be dropped first. What is left is the 68010+ frame --
 *
 *     UWORD SR        %sp@(0)
 *     ULONG PC        %sp@(2)
 *     UWORD format    %sp@(6)
 *
 * -- whose low 12 bits of the format word are the vector number.
 */
void emu68_trap_probe(ULONG id);
asm (
    "   .text\n"
    "   .balign 2\n"
    "   .globl emu68_trap_probe\n"
    "emu68_trap_probe:\n"
    "   addq.l  #4,%sp\n"                       /* drop the Id */
    "   movem.l %d0-%d7/%a0-%a6,%sp@-\n"        /* 15 longs, frame now at 60 */
    "   move.l  %usp,%a0\n"
    "   move.l  %a0,%sp@-\n"                    /* USP first, frame now at 64 */
    "   move.l  %sp,%a0\n"
    "   move.l  %a0,%sp@-\n"                    /* argument: the block above */
    "   jsr     emu68_trap_report\n"            /* does not return */
);

/*
 * Every vector that means "this code is not going to make sense any more".
 * Vector 8 is deliberately absent: Exec_Supervisor_Trap owns it, and it is
 * reinstalled after M68KExceptionInit() anyway.
 */
const struct M68KException emu68_exception_table[] = {
    { .Id =  2, .Handler = emu68_trap_probe },  /* bus error           */
    { .Id =  3, .Handler = emu68_trap_probe },  /* address error       */
    { .Id =  4, .Handler = emu68_trap_probe },  /* illegal instruction */
    { .Id =  5, .Handler = emu68_trap_probe },  /* divide by zero      */
    { .Id =  6, .Handler = emu68_trap_probe },  /* CHK                 */
    { .Id =  7, .Handler = emu68_trap_probe },  /* TRAPV               */
    { .Id = 10, .Handler = emu68_trap_probe },  /* line A              */
    { .Id = 11, .Handler = emu68_trap_probe },  /* line F              */
    { .Id =  0, }
};

#else

const struct M68KException emu68_exception_table[] = {
    { .Id = 0, }
};

#endif /* EMU68_TRAP_PROBE */
