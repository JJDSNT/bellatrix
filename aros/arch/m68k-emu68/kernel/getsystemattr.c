/*
    Copyright (C) 1995-2015, The AROS Development Team. All rights reserved.

    Desc: KrnGetSystemAttr() for the native Emu68 m68k target.
*/

#include <aros/kernel.h>
#include <exec/execbase.h>
#include <proto/exec.h>

/* Order matters: kernel_base.h has to be seen before proto/kernel.h, or the
 * two disagree about the type of KernelBase. Same order as the generic
 * rom/kernel/getsystemattr.c. */
#include <kernel_base.h>
#include <proto/kernel.h>

/*
 * Discovered from /soc's "ranges" by platform/platform.c. Drivers built as
 * their own modules (mbox.resource, sdcard.device) cannot reach the platform
 * layer's statics -- each module is linked with --localize-symbols -- so this
 * is the supported way for them to find the peripheral window, exactly as
 * arch/arm-native does with ARMI_PeripheralBase.
 */
extern ULONG platform_periiobase;

AROS_LH1(intptr_t, KrnGetSystemAttr,
    AROS_LHA(uint32_t, id, D0),
    struct KernelBase *, KernelBase, 29, Kernel)
{
    AROS_LIBFUNC_INIT

    switch (id)
    {
    case KATTR_Architecture:
        return (intptr_t)"m68k-emu68";

    case KATTR_PeripheralBase:
        return (intptr_t)platform_periiobase;

    default:
        return -1;
    }

    AROS_LIBFUNC_EXIT
}
