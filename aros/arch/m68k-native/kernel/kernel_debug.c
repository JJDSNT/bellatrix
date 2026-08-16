/*
 * Low-level debug output for the m68k port.
 */

#include <aros/bootcontract.h>
#include <aros/kernel.h>
#include <aros/symbolsets.h>

#include <kernel_base.h>
#include <kernel_debug.h>

/*
 * The character sink, and its default.
 *
 * <aros/bootcontract.h> asks a machine for an address that absorbs a byte
 * store, so that kprintf()/bug() have somewhere to go before -- and
 * independently of -- any console. Which address that is belongs to the
 * machine: Emu68 leaves 0xdeadbeef unmapped and turns the resulting fault into
 * its own host-side kprintf(), which reaches the real UART and is what QEMU's
 * -serial shows. boot/console.c installs that one.
 *
 * The default discards. A machine that supplies nothing therefore boots and
 * loses the log, which is what the contract says happens, rather than storing
 * to an address chosen by whoever wrote this file.
 *
 * Initialised rather than left in .bss on purpose: this is called before
 * anything has had a chance to clear .bss, and a wild pointer here would
 * fault in the one path that exists to report faults.
 */
static int krnDiscardC(int chr)
{
    return chr;
}

int (*m68k_boot_putc)(int chr) = krnDiscardC;

int krnPutC(int chr, struct KernelBase *KernelBase)
{
    (void)KernelBase;
    return m68k_boot_putc(chr);
}

static void krnPutS(const char *text)
{
    while (*text)
        m68k_boot_putc(*text++);
}

/*
 * Bracket the generic m68k kernel initialization hooks.  These messages are
 * also useful after bring-up: they identify the exact point at which the
 * target-specific kernel services become usable, without relying on the
 * bootstrap's own debugger.
 *
 * They go out through the sink pointer rather than through the bootstrap's
 * puts(). Calling into boot/console.c from here was this half's last link
 * against a particular machine, and it was an unnecessary one: the contract
 * already provides a byte sink that is always callable.
 *
 * The tag says m68k, not Emu68, because this is the port speaking and not the
 * machine. Mixed with boot/boot.c's "[AROS/Emu68]" lines, the boot log now
 * shows which half emitted each message, which is worth more than a uniform
 * prefix.
 */
static int m68k_kernel_init_begin(struct KernelBase *KernelBase)
{
    (void)KernelBase;
    krnPutS("[AROS/m68k] kernel init hooks begin\n");
    return TRUE;
}

static int m68k_kernel_init_end(struct KernelBase *KernelBase)
{
    (void)KernelBase;
    krnPutS("[AROS/m68k] kernel init hooks complete\n");
    return TRUE;
}

/*
 * The INITLIB set is walked from the lowest priority up, so -127 is what runs
 * first and 127 last -- the opposite of the reading that put "hooks complete"
 * ahead of "hooks begin" in the boot log.
 */
ADD2INITLIB(m68k_kernel_init_begin, -127)
ADD2INITLIB(m68k_kernel_init_end, 127)
