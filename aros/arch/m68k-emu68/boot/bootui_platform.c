/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: This machine's answers to what the boot presentation asks for.
*/

#include "boot.h"
#include "bootui_platform.h"

#include <aros/kernel.h>
#include <proto/exec.h>
#include <proto/kernel.h>

/*
 * Where the surface comes from changes during the boot, and the boot UI is
 * not told.
 *
 * Before AROS exists there is only the handover context Emu68 filled in, and
 * that is the whole reason a splash can be drawn at all that early. Once
 * kernel.resource is up the same facts are available through
 * KrnGetSystemAttr(), which is what any other AROS code would ask, so that is
 * what gets asked -- and the day the surface stops arriving from Emu68, this
 * function is the only thing that changes.
 */
int bootui_platform_surface(void **framebuffer, uint32_t *pitch,
                            uint32_t *width, uint32_t *height,
                            uint32_t *depth)
{
    struct Emu68BootContext *ctx = &emu68_boot_context;

    /*
     * Only ask AROS once AROS exists.
     *
     * The boot presentation draws its first frame before ExecBase does, which
     * is the whole reason it can cover that part of the boot -- and it means
     * OpenResource() here is a call through a SysBase that is still whatever
     * was in memory at address 4. Doing it unguarded took the guest to
     * 0xe1031db8 after 62 instructions.
     *
     * ctx->exec_base is set by start_aros() when SysBase is real, so it is
     * the honest test for "is there a system to ask yet".
     */
    if (ctx->exec_base)
    {
        APTR KernelBase = OpenResource("kernel.resource");

        if (KernelBase)
        {
            IPTR fb = KrnGetSystemAttr(KATTR_FrameBuffer);

            if (fb && fb != (IPTR)-1)
            {
                *framebuffer = (void *)fb;
                *pitch  = (uint32_t)KrnGetSystemAttr(KATTR_FrameBufferPitch);
                *width  = (uint32_t)KrnGetSystemAttr(KATTR_FrameBufferWidth);
                *height = (uint32_t)KrnGetSystemAttr(KATTR_FrameBufferHeight);
                *depth  = (uint32_t)KrnGetSystemAttr(KATTR_FrameBufferDepth);
                return 1;
            }
        }
    }

    if (!(ctx->flags & EMU68_BOOT_FRAMEBUFFER))
        return 0;

    *framebuffer = ctx->framebuffer;
    *pitch  = ctx->framebuffer_pitch;
    *width  = ctx->framebuffer_width;
    *height = ctx->framebuffer_height;
    *depth  = 16;
    return 1;
}

void bootui_platform_log(const char *text)
{
    emu68_console_puts(text);
}

const char *bootui_platform_args(uint32_t *length)
{
    if (length)
        *length = emu68_boot_context.bootargs_size;

    return emu68_boot_context.bootargs;
}
