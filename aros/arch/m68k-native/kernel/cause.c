/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.
*/

#include <aros/kernel.h>

#include <kernel_base.h>

#include <proto/kernel.h>

/*
 * Inert because Exec does not call KrnCause() on the uniprocessor path this
 * port takes -- software interrupts are delivered through Exec's own Cause().
 *
 * The earlier reason given here, that no platform interrupt controller ABI
 * existed, has not held since platform.c started discovering one; the reason
 * above is the one that survived and it is about Exec, not about a machine.
 */
AROS_LH0I(void, KrnCause,
    struct KernelBase *, KernelBase, 3, Kernel)
{
    AROS_LIBFUNC_INIT

    AROS_LIBFUNC_EXIT
}

