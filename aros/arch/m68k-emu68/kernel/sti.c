/*
 * See cli.c.  Emu68 interrupt delivery will replace this bootstrap no-op.
 */

#include <aros/kernel.h>
#include <aros/libcall.h>

#include <kernel_base.h>

AROS_LH0I(void, KrnSti,
    struct KernelBase *, KernelBase, 10, Kernel)
{
    AROS_LIBFUNC_INIT

    AROS_LIBFUNC_EXIT
}
