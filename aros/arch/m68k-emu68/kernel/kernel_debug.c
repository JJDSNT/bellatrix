/*
 * Low-level debug output for native Emu68.
 */

#include <aros/kernel.h>
#include <aros/symbolsets.h>

#include <kernel_base.h>
#include <kernel_debug.h>

extern void emu68_console_puts(const char *text);

/*
 * Emu68 leaves 0x00000000deadbeef deliberately unmapped so a one-byte write
 * from the guest faults into its own host-side kprintf(), which reaches the
 * real UART (visible through QEMU's -serial). This gives kprintf()/bug()
 * scrollback console output independent of the m68k framebuffer console.
 */
int krnPutC(int chr, struct KernelBase *KernelBase)
{
    (void)KernelBase;
    *(volatile UBYTE *)0xdeadbeef = (UBYTE)chr;
    return chr;
}

/*
 * Bracket the generic m68k kernel initialization hooks.  These messages are
 * also useful after bring-up: they identify the exact point at which the
 * target-specific kernel services become usable, without relying on Emu68's
 * debugger.
 */
static int emu68_kernel_init_begin(struct KernelBase *KernelBase)
{
    (void)KernelBase;
    emu68_console_puts("[AROS/Emu68] kernel init hooks begin\n");
    return TRUE;
}

static int emu68_kernel_init_end(struct KernelBase *KernelBase)
{
    (void)KernelBase;
    emu68_console_puts("[AROS/Emu68] kernel init hooks complete\n");
    return TRUE;
}

/*
 * The INITLIB set is walked from the lowest priority up, so -127 is what runs
 * first and 127 last -- the opposite of the reading that put "hooks complete"
 * ahead of "hooks begin" in the boot log.
 */
ADD2INITLIB(emu68_kernel_init_begin, -127)
ADD2INITLIB(emu68_kernel_init_end, 127)
