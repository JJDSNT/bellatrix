/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/kernel.h>

#include <kernel_base.h>

#include <proto/kernel.h>

/*
 * Emu68 currently provides no platform interrupt controller ABI to guests.
 * Exec does not call KrnCause() on the initial uniprocessor bootstrap path,
 * so keep this entry point inert until the Emu68 interrupt interface is
 * defined and wired into kernel.resource.
 */
AROS_LH0I(void, KrnCause,
    struct KernelBase *, KernelBase, 3, Kernel)
{
    AROS_LIBFUNC_INIT

    AROS_LIBFUNC_EXIT
}

