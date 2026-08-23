/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: This machine's answers to what the boot presentation asks for.
*/

#include "boot.h"
#include "bootui_platform.h"

#include <aros/kernel.h>
#include <aros/scanout.h>
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
/*
 * The surface last published by whoever programmed a mode. Zero until a
 * display driver says otherwise, which is the normal state for the whole
 * early boot.
 */
static struct
{
    APTR  framebuffer;
    ULONG pitch;
    ULONG width;
    ULONG height;
    ULONG depth;
    /*
     * How many times a driver has said this. Not a value to compare against
     * the last one -- a count of statements made.
     *
     * Programming a mode clears the surface before publishing it, and a driver
     * may well re-publish the same address at the same geometry: same page,
     * same pitch, contents gone. Comparing the values then says "nothing
     * changed" about a surface that has just been wiped, and whoever was
     * drawing there stops repainting a picture that is no longer on screen.
     * That is how the boot presentation ended up as a clock on a black field
     * for the whole hold.
     */
    ULONG generation;
} scanout;

static struct ScanoutResource scanout_resource;

static void scanout_publish(APTR framebuffer, ULONG pitch, ULONG width,
                            ULONG height, ULONG depth)
{
    scanout.framebuffer = framebuffer;
    scanout.pitch       = pitch;
    scanout.width       = width;
    scanout.height      = height;
    scanout.depth       = depth;
    scanout.generation++;
}

static BOOL scanout_current(APTR *framebuffer, ULONG *pitch, ULONG *width,
                            ULONG *height, ULONG *depth)
{
    if (!scanout.framebuffer)
        return FALSE;

    *framebuffer = scanout.framebuffer;
    *pitch       = scanout.pitch;
    *width       = scanout.width;
    *height      = scanout.height;
    *depth       = scanout.depth;
    return TRUE;
}

void bootui_platform_add_scanout_resource(void)
{
    scanout_resource.node.ln_Name = SCANOUT_RESOURCE_NAME;
    scanout_resource.node.ln_Pri  = 0;
    scanout_resource.node.ln_Type = NT_RESOURCE;
    scanout_resource.publish      = scanout_publish;
    scanout_resource.current      = scanout_current;
    AddResource(&scanout_resource.node);
}

int bootui_platform_scanout_changed(void **framebuffer, uint32_t *pitch,
                                    uint32_t *width, uint32_t *height,
                                    uint32_t *depth)
{
    static ULONG seen;

    if (!scanout.framebuffer || scanout.generation == seen)
        return 0;

    seen         = scanout.generation;
    *framebuffer = scanout.framebuffer;
    *pitch       = scanout.pitch;
    *width       = scanout.width;
    *height      = scanout.height;
    *depth       = scanout.depth;
    return 1;
}

int bootui_platform_surface(void **framebuffer, uint32_t *pitch,
                            uint32_t *width, uint32_t *height,
                            uint32_t *depth)
{
    struct Emu68BootContext *ctx = &emu68_boot_context;

    /*
     * A driver that has programmed a mode has the only current answer, and it
     * outranks both of the ones below: the kernel still reports the surface
     * the bootloader handed over, and so does the handover context.
     */
    if (scanout.framebuffer)
    {
        *framebuffer = scanout.framebuffer;
        *pitch       = scanout.pitch;
        *width       = scanout.width;
        *height      = scanout.height;
        *depth       = scanout.depth;
        return 1;
    }

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
