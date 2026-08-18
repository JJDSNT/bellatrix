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

/*
 * The framebuffer Emu68 handed over, republished as kernel attributes.
 *
 * AROS already defines KATTR_FrameBuffer and its companions (aros/kernel.h),
 * and arch/aarch64-raspi's fbgfx driver reads the display it drives from
 * exactly those -- initFBGfxHW() in hidd/fbgfx/fbgfx_support.c does nothing
 * else to find the surface. Answering them here is what lets that driver be
 * ported to this target without its hardware layer having to know anything
 * about Emu68.
 *
 * Reaching emu68_boot_context from a different module is already established
 * practice here: hidd/emu68gfx/emu68gfx_init.c does it, with the boot
 * directory on its include path.
 */
#include <boot.h>

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

    /*
     * Zero rather than -1 when there is no framebuffer: a caller that only
     * checks for -1 then still sees "nothing", and fbgfx checks for both.
     */
    case KATTR_FrameBuffer:
        if (!(emu68_boot_context.flags & EMU68_BOOT_FRAMEBUFFER))
            return 0;
        return (intptr_t)emu68_boot_context.framebuffer;

    case KATTR_FrameBufferWidth:
        return (intptr_t)emu68_boot_context.framebuffer_width;

    case KATTR_FrameBufferHeight:
        return (intptr_t)emu68_boot_context.framebuffer_height;

    case KATTR_FrameBufferPitch:
        return (intptr_t)emu68_boot_context.framebuffer_pitch;

    /*
     * 16, not 32. Emu68 hands over an RGB16_LE surface -- little-endian
     * 5:6:5 on a big-endian CPU -- where the aarch64 target's firmware
     * framebuffer is 32bpp R,G,B,x. Any driver ported from there has to be
     * told this rather than assuming its own case.
     */
    case KATTR_FrameBufferDepth:
        return 16;

    default:
        return -1;
    }

    AROS_LIBFUNC_EXIT
}
