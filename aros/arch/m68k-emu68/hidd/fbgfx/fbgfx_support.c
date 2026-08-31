/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: Linear framebuffer gfx HIDD. The bootstrap brings the VideoCore
          framebuffer up and hands its geometry to the kernel; this driver
          wraps that surface, querying it via KrnGetSystemAttr. It touches no
          VideoCore register itself - the one board-specific thing left is the
          pixel byte order below, which the firmware fixes and no attribute
          reports.
*/

#define DEBUG 0
#include <aros/debug.h>
#include <aros/kernel.h>
#include <proto/exec.h>

/* kernel.resource is a resource, not a library: open it explicitly rather
   than letting the module's autoinit try to OpenLibrary() it. */
#define __NOLIBBASE__
#include <proto/kernel.h>
#include <string.h>

#include "fbgfx_intern.h"
#include "fbgfx_hidd.h"
#ifdef __EMU68__
#include <bootui_api.h>
#endif

BOOL initFBGfxHW(struct HWData *data)
{
    struct KernelBase *KernelBase = OpenResource("kernel.resource");
    IPTR fb = KernelBase ? (IPTR)KrnGetSystemAttr(KATTR_FrameBuffer) : 0;

    /* KrnGetSystemAttr() answers -1 for anything it does not know, so a
       kernel without the framebuffer attributes hands back a pointer that
       is not NULL but is not memory either. */
    if (fb == 0 || fb == (IPTR)-1)
    {
        D(bug("[FBGfx] HwInit: framebuffer not available\n"));
        return FALSE;
    }

#if !defined(DEBUGDISPLAY)
    /*
     * Detach the bootstrap's framebuffer console before handing this surface
     * on. 0x03 to RawPutChar drops ARMI_PutChar (krnPutC in
     * arch/aarch64-native/kernel/kernel_debug.c), leaving debug output on the
     * serial line; without it every bug() keeps drawing characters into the
     * screen Workbench is using.
     */
    RawPutChar(0x03);
#endif

    data->framebuffer  = (APTR)fb;
    data->width        = KrnGetSystemAttr(KATTR_FrameBufferWidth);
    data->height       = KrnGetSystemAttr(KATTR_FrameBufferHeight);
    data->bytesperline = KrnGetSystemAttr(KATTR_FrameBufferPitch);

    /*
     * Ask, rather than assume. This driver already takes width, height and
     * pitch from the kernel; the depth was the one thing it hard-coded, which
     * made it a driver for one bootloader's choice instead of for a linear
     * framebuffer. A kernel with nothing to say answers -1, and 32bpp is what
     * this file assumed before, so that stays the default.
     */
    {
        IPTR d = KrnGetSystemAttr(KATTR_FrameBufferDepth);

        if (d != 16 && d != 32)
            d = 32;
        data->depth = data->bitsperpixel = d;
        data->bytesperpixel = d >> 3;
    }

    /*
     * AROS's MAP_COLCOMP shift convention is (mask << shift) == 0xFF000000 --
     * the component ends up in the top byte -- so the shift is *not* the
     * mask's own bit position.
     */
    if (data->bitsperpixel == 16)
    {
        /*
         * 5:6:5 in a halfword. Emu68 hands this over on the Raspberry Pi and
         * calls it RGB16_LE; the VideoCore stores it little-endian whatever
         * the CPU is, so on a big-endian target every pixel needs its two
         * bytes swapped on the way through. Getting this wrong is not subtle
         * and not silent -- every colour comes out wrong.
         */
        data->redmask   = 0x0000F800; data->redshift   = 16;
        data->greenmask = 0x000007E0; data->greenshift = 21;
        data->bluemask  = 0x0000001F; data->blueshift  = 27;
        data->palettewidth = 8;
        data->swappixelbytes = AROS_BIG_ENDIAN ? TRUE : FALSE;
    }
    else
    {
        /*
         * The VideoCore surface displays byte order R,G,B,x (see fb.c put()),
         * so the pixel masks are red@0-7, green@8-15, blue@16-23. Byte order
         * rather than word layout, so there is nothing to swap.
         */
        data->redmask   = 0x000000FF; data->redshift   = 24;
        data->greenmask = 0x0000FF00; data->greenshift = 16;
        data->bluemask  = 0x00FF0000; data->blueshift  = 8;
        data->palettewidth = 8;
        data->swappixelbytes = FALSE;
    }
    data->fbsize = data->height * data->bytesperline;

    /*
     * Unconditional, unlike everything else here.
     *
     * Every other message in this driver is D() and compiled out, so a boot
     * log had nothing at all to say about the display -- not which driver
     * initialised, not at what mode, not where. That is one line's worth of
     * silence hiding the newest component in the system, and it made a
     * question as basic as "which graphics driver is this boot running?"
     * answerable only by remembering what was compiled.
     */
    bug("[FBGfx] %ux%u %ubpp linear FB @ 0x%p, pitch %u%s\n",
        data->width, data->height, data->bitsperpixel,
        data->framebuffer, data->bytesperline,
        data->swappixelbytes ? ", byte-swapped" : "");

#ifdef __EMU68__
    /*
     * Not here on this target. m68k-emu68's bootstrap is still drawing its
     * splash on this surface and does not give it up until the first Show()
     * (see the __EMU68__ block in fbgfx_displayclass.c), so clearing at driver
     * init wipes the artwork while the boot is still running -- and only the
     * artwork, because the progress band is repainted on every stage change
     * and comes back. The symptom is a loader floating on black.
     *
     * The aarch64 path clears because it has already detached its framebuffer
     * console above, so by this point nothing else owns the surface.
     */
#else
    ClearBuffer(data);
#endif
    return TRUE;
}

/* Copy the (possibly partial) bitmap buffer to the visible framebuffer. */
void fbDoRefreshArea(struct HWData *hwdata, struct FBGfxBitMapData *data,
                       LONG x1, LONG y1, LONG x2, LONG y2)
{
    UBYTE *src, *dst;
    ULONG srcmod, dstmod;
    LONG y, w, h, sx, sy;

#ifdef __EMU68__
    /*
     * The boot splash still owns the framebuffer. Everything drawn now lands
     * in the bitmap's own buffer and stays there.
     */
    if (bootui_holding())
        return;

    /*
     * The hold has just ended and nothing of the desktop has reached the
     * framebuffer, so the first refresh after it has to be the whole screen
     * rather than whatever rectangle the caller asked for.
     *
     * Collected here rather than pushed from the timer that ends the hold:
     * that runs in an interrupt, where taking the framebuffer semaphore is
     * illegal -- the version that did it earned two "called in supervisor
     * mode" alerts from Wanderer.
     */
    if (bootui_take_release())
    {
        x1 = 0; y1 = 0;
        x2 = data->width; y2 = data->height;
    }
#endif

    x1 += data->xoffset; y1 += data->yoffset;
    x2 += data->xoffset; y2 += data->yoffset;

    if ((x1 >= data->disp_width) || (x2 < 1) ||
        (y1 >= data->disp_height) || (y2 < 1))
        return;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > data->disp_width)  x2 = data->disp_width;
    if (y2 > data->disp_height) y2 = data->disp_height;

    w = x2 - x1;
    h = y2 - y1;
    sx = x1 - data->xoffset;
    sy = y1 - data->yoffset;
    w *= data->bytesperpix;

    srcmod = data->bytesperline;
    dstmod = hwdata->bytesperline;
    src = data->VideoData + sy * data->bytesperline + sx * data->bytesperpix;
    dst = (UBYTE *)hwdata->framebuffer + y1 * hwdata->bytesperline + x1 * hwdata->bytesperpixel;

    if ((srcmod != dstmod) || (srcmod != (ULONG)w))
    {
        for (y = 0; y < h; y++)
        {
            CopyMem(src, dst, w);
            src += srcmod;
            dst += dstmod;
        }
    }
    else
    {
        CopyMem(src, dst, w * h);
    }
}

/* Truecolor only: no hardware palette to load. */
void DACLoad(struct FBGfx_staticdata *xsd, UBYTE *DAC, unsigned char first, int num)
{
    (void)xsd; (void)DAC; (void)first; (void)num;
}

void ClearBuffer(struct HWData *data)
{
    if (data->framebuffer)
        memset(data->framebuffer, 0, data->height * data->bytesperline);
}
