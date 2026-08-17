/*
 * Emu68 owns the host interrupt controller.  Until its interrupt service
 * vectors are wired to kernel.resource, Exec's nesting counters provide the
 * serialization required by the chipset-free bootstrap.
 */

#include <aros/kernel.h>
#include <aros/libcall.h>

#include <kernel_base.h>

AROS_LH0I(void, KrnCli,
    struct KernelBase *, KernelBase, 9, Kernel)
{
    AROS_LIBFUNC_INIT

    AROS_LIBFUNC_EXIT
}
