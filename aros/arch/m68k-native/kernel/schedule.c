/* Exec scheduler entry for the m68k port. */

#include <aros/kernel.h>

#include <kernel_base.h>
#include <kernel_syscall.h>

#include <proto/kernel.h>

AROS_LH0(void, KrnSchedule,
    struct KernelBase *, KernelBase, 6, Kernel)
{
    AROS_LIBFUNC_INIT

    Supervisor(__AROS_GETVECADDR(SysBase, 7));

    AROS_LIBFUNC_EXIT
}
