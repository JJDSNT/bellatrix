/*
 * Deliberately empty, and not a placeholder.
 *
 * This used to say that it was one -- "until Emu68's interrupt service vectors
 * are wired to kernel.resource" -- which stopped being true once platform.c
 * installed all seven autovectors. The reason it does nothing is simpler and
 * permanent: on this port the physical IRQ gate is the SR mask. An external
 * controller drives the level, the CPU arbitrates it against the mask, and the
 * mask is restored by the RTE. There is no separate enable to clear here, so
 * KrnCli()/KrnSti() are not the gate and must not pretend to be; Exec's own
 * nesting counters carry the serialization.
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
