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
 * Watch the Exec jump table.
 *
 * Built to confirm that the table was being overwritten, and it refuted that
 * instead -- which is why it stays. Across eight runs it reports exactly four
 * changes and they are identical in the boots that reach the icons and the
 * boots that die: AROS patching its own vectors during startup. The table is
 * intact, and the fatal calls go through an A6 that is garbage rather than a
 * vector that is. See AI_context/issues/ISSUE-0007.md.
 *
 * It stays because "the table is still intact" is worth knowing on every run
 * that fails, and because it is the only thing here that measures where
 * SysBase actually is -- which is what showed the earlier reading of
 * A6-PC = 132 to be a coincidence.
 *
 * The obvious instrument is the MMU: mark the page read-only and let the write
 * fault, naming its author. It cannot be used here. The jump table sits *below*
 * SysBase and SysBase's own mutable fields sit above it, in the same 4 KiB
 * page, and IDNestCnt is written by every Forbid/Permit pair in the system.
 * Trapping that page means a data abort on the hottest write in Exec -- the
 * same shape as the 262144-abort sweep recorded in docs/legacy-emu68-patches.md.
 *
 * So compare instead of trap. A copy of the table costs a kilobyte and a
 * memcmp per tick costs nothing, and it answers a narrower question than the
 * trap probe does: not "who called through the broken vector" but "when did it
 * break, and what was running". The boot is allowed to
 * continue -- the interesting part is often what happens next.
 */
#define LVO_GUARD_MAX 1024

static UBYTE lvo_guard_copy[LVO_GUARD_MAX];
static UBYTE *lvo_guard_base;
static ULONG lvo_guard_size;
static ULONG lvo_guard_events;

/* The platform tick, so a report says *when* as well as what. */
extern volatile ULONG emu68_platform_ticks;

/*
 * Report several changes, not the first one.
 *
 * The first version stopped after one, and spent it on a legitimate write:
 * AROS patches its own vectors during startup -- m68k_ExecInstallPreserveAll()
 * is called from boot.c and SetFunction() is used elsewhere -- so the guard
 * fired identically in every run, including the ones that reach the icons, and
 * then went quiet before anything interesting happened.
 *
 * Arming later would be guesswork about when the legitimate patching ends, and
 * would miss a corruption that happens before that point -- two of eight runs
 * died before the screen even opened. So report each change and re-sync, and
 * let the comparison between a good boot and a bad one do the work: the
 * legitimate ones appear in both.
 */
#define LVO_GUARD_EVENTS 8

void emu68_lvo_guard_arm(void)
{
    struct ExecBase *sb = *(struct ExecBase **)4;
    ULONG size, i;

    if (!sb)
        return;

    size = sb->LibNode.lib_NegSize;
    if (size == 0 || size > LVO_GUARD_MAX)
        size = LVO_GUARD_MAX;

    lvo_guard_base = (UBYTE *)sb - size;
    lvo_guard_size = size;

    for (i = 0; i < size; i++)
        lvo_guard_copy[i] = lvo_guard_base[i];

    emu68_console_puts("[AROS/Emu68] LVO guard armed over 0x");
    puthex((ULONG)lvo_guard_base);
    emu68_console_puts("-0x");
    puthex((ULONG)(lvo_guard_base + size - 1));
    emu68_console_puts("\n");
}

void emu68_lvo_guard_check(void)
{
    struct ExecBase *sb;
    struct Task *task;
    ULONG i;

    if (!lvo_guard_base || lvo_guard_events >= LVO_GUARD_EVENTS)
        return;

    for (i = 0; i < lvo_guard_size; i++)
        if (lvo_guard_base[i] != lvo_guard_copy[i])
            break;

    if (i == lvo_guard_size)
        return;

    lvo_guard_events++;
    sb = *(struct ExecBase **)4;
    task = sb ? sb->ThisTask : (struct Task *)0;

    emu68_console_puts("[AROS/Emu68] LVO GUARD #");
    puthex(lvo_guard_events);
    emu68_console_puts(" tick 0x");
    puthex(emu68_platform_ticks);
    emu68_console_puts(": changed at 0x");
    puthex((ULONG)&lvo_guard_base[i]);
    emu68_console_puts(" (LVO -");
    puthex(lvo_guard_size - i);
    emu68_console_puts(")\n  was 0x");
    puthex(lvo_guard_copy[i]);
    emu68_console_puts("  now 0x");
    puthex(lvo_guard_base[i]);
    emu68_console_puts("\n  task 0x");
    puthex((ULONG)task);
    if (task && task->tc_Node.ln_Name)
    {
        emu68_console_puts(" '");
        emu68_console_puts(task->tc_Node.ln_Name);
        emu68_console_puts("'");
    }
    emu68_console_puts("\n");

    /*
     * Print the whole changed run, not just its first byte: what was written
     * is as much of a fingerprint as where. A string, a node pointer and a
     * length field each look quite different.
     */
    emu68_console_puts("  bytes:");
    for (; i < lvo_guard_size && i < LVO_GUARD_MAX; i++)
    {
        if (lvo_guard_base[i] == lvo_guard_copy[i])
            continue;
        emu68_console_puts(" 0x");
        puthex(lvo_guard_base[i]);
    }
    emu68_console_puts("\n");

    /* Re-sync, so the next change is a change from here rather than a repeat
     * of this one. */
    for (i = 0; i < lvo_guard_size; i++)
        lvo_guard_copy[i] = lvo_guard_base[i];
}

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

static void dump_stack(const char *what, ULONG sp)
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

    for (i = 0; i < 24; i++)
    {
        ULONG v = p[i];

        emu68_console_puts("    +0x");
        puthex((ULONG)(i * 4));
        emu68_console_puts(" 0x");
        puthex(v);

        if (v >= (ULONG)__aros_resident_start && v < (ULONG)__aros_resident_end)
            emu68_console_puts("  <- kernel");

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

    dump_stack("  SSP 0x", (ULONG)&regs[16] + 8);
    dump_stack("  USP 0x", regs[0]);

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
