/*
    Copyright (C) 2013-2026, The AROS Development Team. All rights reserved.

    Desc: BCM VideoCore4 Gfx Hidd Onscreen Bitmap Class.
*/

#define DEBUG 0
#include <aros/debug.h>

#define __OOP_NOATTRBASES__

#include <proto/oop.h>
#include <proto/mbox.h>
#ifdef __EMU68__
#include <aros/scanout.h>
#include <aros/bootui.h>
#endif
#include <proto/utility.h>
#include <assert.h>
#include <exec/memory.h>
#include <exec/lists.h>
#include <aros/symbolsets.h>
#include <graphics/rastport.h>
#include <graphics/gfx.h>
#include <hidd/gfx.h>
#include <oop/oop.h>
#include <stddef.h>

#include "vcgfx_bitmap.h"
#include "vcgfx_hidd.h"
#include "vcgfx_hvs.h"

#include LC_LIBDEFS_FILE

#ifdef MBoxBase
#undef MBoxBase
#endif

#define MBoxBase      (&((struct VideoCoreGfxBase *)cl->UserData)->vsd)->vcsd_MBoxBase

#define MNAME_ROOT(x) VideoCoreGfxOnBM__Root__ ## x
#define MNAME_BM(x) VideoCoreGfxOnBM__Hidd_BitMap__ ## x

#define OnBitmap 1
#include "vcgfx_bitmapclass.c"

/* Included bitmapclass.c sets its own DEBUG; reset to this file's value. */
#undef DEBUG
#define DEBUG 0

/* bitmapclass.c's MBoxBase resolves through 'cl'; the helpers below carry
 * 'xsd' instead, so switch the macro to pull MBoxBase off it directly.
 */
#undef MBoxBase
#define MBoxBase      xsd->vcsd_MBoxBase

/* Program the scanout window offset within the virtual framebuffer.
 * Returns TRUE only if the firmware echoes the requested offset back.
 */
static BOOL vc4_set_voffset(struct VideoCoreGfx_staticdata *xsd, ULONG yoffset)
{
    BOOL ok;

    VC4_MBOX_LOCK(xsd);
    xsd->vcsd_MBoxMessage[0] = AROS_LE2LONG(8 * 4);
    xsd->vcsd_MBoxMessage[1] = AROS_LE2LONG(VCTAG_REQ);
    xsd->vcsd_MBoxMessage[2] = AROS_LE2LONG(VCTAG_SETVOFFSET);
    xsd->vcsd_MBoxMessage[3] = AROS_LE2LONG(8);
    xsd->vcsd_MBoxMessage[4] = AROS_LE2LONG(8);
    xsd->vcsd_MBoxMessage[5] = AROS_LE2LONG(0);
    xsd->vcsd_MBoxMessage[6] = AROS_LE2LONG(yoffset);
    xsd->vcsd_MBoxMessage[7] = 0;

    ok = (MBoxCall((void *)VCMB_BASE, VCMB_PROPCHAN, xsd->vcsd_MBoxMessage)
            != (volatile unsigned int *)-1)
        && (xsd->vcsd_MBoxMessage[4] == AROS_LE2LONG(VCTAG_RESP + 8))
        && (AROS_LE2LONG(xsd->vcsd_MBoxMessage[6]) == yoffset);
    VC4_MBOX_UNLOCK(xsd);

    return ok;
}

/* Phys address of the back page, or 0 when flipping is unavailable. */
ULONG vc4_fb_backpage(struct VideoCoreGfx_staticdata *xsd)
{
    if (xsd->vcsd_FBPages < 2)
        return 0;
    return xsd->vcsd_FBPage[1 - xsd->vcsd_FBFront];
}

/* Make the back page visible and retarget rendering (data->VideoData and
 * the ChunkyBM buffer) at it. Caller must have drawn the full frame first.
 */
BOOL vc4_fb_flip(struct VideoCoreGfx_staticdata *xsd)
{
    UBYTE nf;

    if (xsd->vcsd_FBPages < 2 || !xsd->vcsd_FBObj)
        return FALSE;

    /* The mailbox lock also serializes concurrent flippers; the nested
     * Obtain inside vc4_set_voffset is fine for the owning task. */
    VC4_MBOX_LOCK(xsd);

    nf = 1 - xsd->vcsd_FBFront;
    if (!vc4_hvs_flip_page(xsd, xsd->vcsd_FBPage[nf])
        && !vc4_set_voffset(xsd, nf * xsd->vcsd_FBPageHeight))
    {
        /* Firmware refused — disable flipping for good. Loud: a GL
         * client that already wrapped the scanout pages keeps rendering
         * to the back page and the screen freezes on the front one. */
        bug("[VideoCoreGfx] flip: SETVOFFSET refused, flipping disabled\n");
        xsd->vcsd_FBPages = 1;
        VC4_MBOX_UNLOCK(xsd);
        return FALSE;
    }
    xsd->vcsd_FBFront = nf;

    {
        OOP_Class *bmcl = xsd->vcsd_VideoCoreGfxOnBMClass;
        struct BitmapData *data = OOP_INST_DATA(bmcl, xsd->vcsd_FBObj);
        struct TagItem btags[] =
        {
            { xsd->vcsd_attrBases[2] + aoHidd_ChunkyBM_Buffer, xsd->vcsd_FBPage[nf] },
            { TAG_DONE, 0 }
        };

        data->VideoData = (UBYTE *)(IPTR)xsd->vcsd_FBPage[nf];
        OOP_SetAttrs(xsd->vcsd_FBObj, btags);
    }

    VC4_MBOX_UNLOCK(xsd);
    return TRUE;
}

#ifdef __EMU68__
/*
 * Assembling a screen out of sight, so a boot presentation can stay up.
 *
 * This driver renders straight into the surface being scanned out, so a
 * screen opening while the presentation is still showing would paint its
 * half-built desktop over it -- and the presentation could not cover that,
 * because there is only one surface and they share it. Ending the
 * presentation at the mode change instead is what used to happen, and it puts
 * several seconds of desktop assembly on screen, which is the thing a boot
 * presentation exists to avoid.
 *
 * The framebuffer already has two pages when the firmware honours SETVOFFSET,
 * for flipping. So point *rendering* at the page that is not being scanned
 * out and leave the scanout where the presentation is drawing. Wanderer then
 * builds a whole desktop nobody can see, and putting it up is one flip.
 *
 * The driver states the capability and never learns what a boot is; the
 * presentation decides whether and how long to hold, and never learns what a
 * page is.
 */
static struct VideoCoreGfx_staticdata *vcgfx_held_xsd;

static void vcgfx_bootui_released(void)
{
    struct VideoCoreGfx_staticdata *xsd = vcgfx_held_xsd;
    struct BitmapData *data;

    if (!xsd || !xsd->vcsd_FBObj)
        return;

    data = OOP_INST_DATA(xsd->vcsd_VideoCoreGfxOnBMClass, xsd->vcsd_FBObj);

    /*
     * Idempotent on purpose. The presentation calls this again a moment after
     * the release, to catch a screen title bar that finishes drawing late, and
     * a second flip would put the wrong page back up. Nothing to do once what
     * is rendered is what is shown.
     */
    if (data->VideoData == (UBYTE *)(IPTR)xsd->vcsd_FBPage[xsd->vcsd_FBFront])
        return;

    /*
     * Publish by moving the scanout, where that is real.
     *
     * vc4_hvs_flip_page() rewrites the fb plane's pointer in the live display
     * list and the hardware latches it at frame start. SETVOFFSET is the
     * fallback and it cannot be trusted: QEMU's firmware emulation stores the
     * value and hands it back, so the round-trip test at mode-set time passes
     * while the display never moves.
     */
    if (xsd->vcsd_HVS.hvs_Active && vc4_fb_flip(xsd))
        return;

    /*
     * Otherwise publish by copying, which is what fbgfx does and works
     * wherever memory does.
     *
     * One pass over the framebuffer puts the finished desktop on the page
     * being scanned out; rendering then goes back to that page, so everything
     * drawn afterwards -- a screen title bar finishing late, the first mouse
     * move -- lands where it can be seen. That is why this needs no settling
     * period and no second copy: after it, what is rendered is what is shown,
     * which is the same property the flip has.
     */
    {
        ULONG front = xsd->vcsd_FBPage[xsd->vcsd_FBFront];
        struct TagItem btags[] =
        {
            { xsd->vcsd_attrBases[2] + aoHidd_ChunkyBM_Buffer, front },
            { TAG_DONE, 0 }
        };

        CopyMem(data->VideoData, (APTR)(IPTR)front,
                data->bytesperrow * data->height);

        data->VideoData = (UBYTE *)(IPTR)front;
        OOP_SetAttrs(xsd->vcsd_FBObj, btags);
    }
}

/*
 * Render off the scanned-out page while the presentation still owns it.
 * Returns TRUE if rendering was moved, so the caller knows a hold is possible.
 */
static BOOL vcgfx_assemble_offscreen(struct VideoCoreGfx_staticdata *xsd,
                                     OOP_Object *o, struct BitmapData *data)
{
    struct BootUIResource *bootui =
        (struct BootUIResource *)OpenResource(BOOTUI_RESOURCE_NAME);
    ULONG back;

    if (!bootui || !bootui->active || !bootui->set_release_hook)
        return FALSE;
    if (!bootui->active())
        return FALSE;

    /*
     * All this needs is somewhere out of sight to draw. Whether the finished
     * screen is then published by moving the scanout or by copying it is
     * decided at release time -- see vcgfx_bootui_released() -- because only
     * one of the two can be trusted on any given machine and neither changes
     * what happens here.
     */
    back = vc4_fb_backpage(xsd);
    if (!back)
        return FALSE;

    {
        struct TagItem btags[] =
        {
            { xsd->vcsd_attrBases[2] + aoHidd_ChunkyBM_Buffer, back },
            { TAG_DONE, 0 }
        };

        data->VideoData = (UBYTE *)(IPTR)back;
        OOP_SetAttrs(o, btags);
    }

    vcgfx_held_xsd = xsd;
    bootui->set_release_hook(vcgfx_bootui_released, BOOTUI_RELEASE_INSTANT);
    return TRUE;
}
#endif /* __EMU68__ */

/* Drive the firmware framebuffer to the requested geometry; report back the
 * CPU pointer and scanout pitch. Used by BitMap::New and BitMap::Set.
 *
 * A double-height virtual framebuffer is requested first; the second page
 * enables SETVOFFSET flipping. Falls back to a single page if unavailable.
 */
static BOOL vc4_program_fb(struct VideoCoreGfx_staticdata *xsd,
    ULONG aligned_width, ULONG height, UBYTE bytesperpix,
    APTR *fb_ptr_out, ULONG *fb_pitch_out)
{
    ULONG bitsperpixel = bytesperpix * 8;
    APTR fb_ptr = NULL;
    ULONG fb_pitch = 0, fb_size = 0;
    int pages;

    /* Mode is changing: the FBALLOC sequence below makes the firmware
     * rebuild its display list and repoint the channel, so give up list
     * ownership first; it is re-taken once the new framebuffer is up. */
    VC4_MBOX_LOCK(xsd);
    xsd->vcsd_HVS.hvs_Active = FALSE;
    VC4_MBOX_UNLOCK(xsd);

    for (pages = 2; pages >= 1; pages--)
    {
        /* Hold the mailbox lock across the whole multi-transaction sequence
         * (FBFREE -> batched mode/depth/pixfmt/FBALLOC -> GETPITCH). */
        VC4_MBOX_LOCK(xsd);

        /*
         * No FBFREE first.
         *
         * Releasing the firmware's framebuffer before knowing a replacement
         * can be had is what turns a failed allocation into a machine with no
         * display at all: the firmware drops the surface *and* the display
         * list that scanned it out, and if FBALLOC then comes back
         * empty-handed there is nothing left to put back. On a Pi 3 that is
         * exactly what happened -- black screen, HDMI channel enabled and
         * running, and its display list a single END word.
         *
         * FBALLOC reallocates over an existing framebuffer on its own, so the
         * FBFREE bought nothing and cost the one thing that cannot be
         * recovered from here.
         */

        /* SETRES + SETVRES + SETDEPTH + SETPIXFMT + FBALLOC in one batch.
         * SETPIXFMT pins channel order to RGB to match pftags_32bpp (R in
         * low bits); real Pi3 defaults to BGR (brown/blue tint), QEMU to RGB.
         */
        xsd->vcsd_MBoxMessage[1]  = AROS_LE2LONG(VCTAG_REQ);
        xsd->vcsd_MBoxMessage[2]  = AROS_LE2LONG(VCTAG_SETRES);
        xsd->vcsd_MBoxMessage[3]  = AROS_LE2LONG(8);
        xsd->vcsd_MBoxMessage[4]  = AROS_LE2LONG(8);
        xsd->vcsd_MBoxMessage[5]  = AROS_LE2LONG(aligned_width);
        xsd->vcsd_MBoxMessage[6]  = AROS_LE2LONG(height);

        xsd->vcsd_MBoxMessage[7]  = AROS_LE2LONG(VCTAG_SETVRES);
        xsd->vcsd_MBoxMessage[8]  = AROS_LE2LONG(8);
        xsd->vcsd_MBoxMessage[9]  = AROS_LE2LONG(8);
        xsd->vcsd_MBoxMessage[10] = AROS_LE2LONG(aligned_width);
        xsd->vcsd_MBoxMessage[11] = AROS_LE2LONG(height * pages);

        xsd->vcsd_MBoxMessage[12] = AROS_LE2LONG(VCTAG_SETDEPTH);
        xsd->vcsd_MBoxMessage[13] = AROS_LE2LONG(4);
        xsd->vcsd_MBoxMessage[14] = AROS_LE2LONG(4);
        xsd->vcsd_MBoxMessage[15] = AROS_LE2LONG(bitsperpixel);

        xsd->vcsd_MBoxMessage[16] = AROS_LE2LONG(VCTAG_SETPIXFMT);
        xsd->vcsd_MBoxMessage[17] = AROS_LE2LONG(4);
        xsd->vcsd_MBoxMessage[18] = AROS_LE2LONG(4);
        xsd->vcsd_MBoxMessage[19] = AROS_LE2LONG(VCPXFMT_BGR);

        xsd->vcsd_MBoxMessage[20] = AROS_LE2LONG(VCTAG_FBALLOC);
        xsd->vcsd_MBoxMessage[21] = AROS_LE2LONG(8);
        xsd->vcsd_MBoxMessage[22] = AROS_LE2LONG(4);
        xsd->vcsd_MBoxMessage[23] = AROS_LE2LONG(16);
        xsd->vcsd_MBoxMessage[24] = 0;

        xsd->vcsd_MBoxMessage[25] = 0;
        xsd->vcsd_MBoxMessage[0]  = AROS_LE2LONG(26 << 2);

        if ((MBoxCall((void *)VCMB_BASE, VCMB_PROPCHAN, xsd->vcsd_MBoxMessage)
                == (volatile unsigned int *)-1)
            || (xsd->vcsd_MBoxMessage[1] != AROS_LE2LONG(VCTAG_RESP))
            || (xsd->vcsd_MBoxMessage[22] != AROS_LE2LONG(VCTAG_RESP + 8)))
        {
            VC4_MBOX_UNLOCK(xsd);
            if (pages > 1)
                continue;
            return FALSE;
        }
        fb_ptr = (APTR)(IPTR)(AROS_LE2LONG(xsd->vcsd_MBoxMessage[23]) & 0x3fffffff);
        fb_size = AROS_LE2LONG(xsd->vcsd_MBoxMessage[24]);

        /*
         * A tagged reply with a null pointer is the firmware saying it could
         * not allocate -- not enough left in the VideoCore split for the mode
         * asked for. That is a failure, and it used to read as success: the
         * caller carried on with a NULL framebuffer, which on this port then
         * quietly fell back to the kernel's boot surface and blamed the HVS
         * for showing nothing.
         */
        if (!fb_ptr || !fb_size)
        {
            bug("[VideoCoreGfx] FBALLOC refused %ux%u %ubpp, %d page(s): "
                "ptr=0x%p size=%u\n", aligned_width, height * pages,
                (unsigned)bitsperpixel, pages, fb_ptr, (unsigned)fb_size);
            VC4_MBOX_UNLOCK(xsd);
            if (pages > 1)
                continue;
            return FALSE;
        }

        /* Use the firmware-reported pitch verbatim: it may align rows, or
         * (QEMU) keep a default virtual width that ignores our request.
         */
        xsd->vcsd_MBoxMessage[0] = AROS_LE2LONG(7 * 4);
        xsd->vcsd_MBoxMessage[1] = AROS_LE2LONG(VCTAG_REQ);
        xsd->vcsd_MBoxMessage[2] = AROS_LE2LONG(VCTAG_GETPITCH);
        xsd->vcsd_MBoxMessage[3] = AROS_LE2LONG(4);
        xsd->vcsd_MBoxMessage[4] = 0;
        xsd->vcsd_MBoxMessage[5] = 0;
        xsd->vcsd_MBoxMessage[6] = 0;

        if ((MBoxCall((void *)VCMB_BASE, VCMB_PROPCHAN, xsd->vcsd_MBoxMessage)
                != (volatile unsigned int *)-1)
            && (xsd->vcsd_MBoxMessage[4] == AROS_LE2LONG(VCTAG_RESP + 4)))
        {
            fb_pitch = AROS_LE2LONG(xsd->vcsd_MBoxMessage[5]);
        }
        else
        {
            /* Fallback: assume packed rows. */
            fb_pitch = aligned_width * bytesperpix;
        }

        VC4_MBOX_UNLOCK(xsd);

        /* Did the allocation really cover both pages? */
        if (pages > 1 && fb_size < fb_pitch * height * 2)
            continue;
        break;
    }

    xsd->vcsd_FBPage[0]    = (ULONG)(IPTR)fb_ptr;
    xsd->vcsd_FBPage[1]    = (ULONG)(IPTR)fb_ptr + fb_pitch * height;
    xsd->vcsd_FBPageHeight = height;
    xsd->vcsd_FBFront      = 0;
    xsd->vcsd_FBPages      = 1;

    /*
     * Clear the page that is about to be scanned out.
     *
     * Whatever was on this display belonged to whoever had it before, and it
     * was drawn for a different mode. The firmware can hand back the same
     * memory at a new depth -- 16bpp to 32bpp here -- and then those bytes are
     * not old content, they are noise: the same pixels reappear as two
     * half-width copies of themselves, which is exactly what a Pi 3 and QEMU
     * both showed after this driver took over.
     *
     * A driver that programs a mode and leaves the previous owner's bytes on
     * screen is displaying something it did not draw. Clearing is the only
     * honest thing it can do with them, and it costs one pass over the
     * framebuffer at mode-set time. The back page below is cleared for the
     * same reason, and has been all along.
     */
    memset((APTR)(IPTR)xsd->vcsd_FBPage[0], 0, fb_pitch * height);

    if (pages == 2)
    {
        /* Only trust flipping if the firmware honours SETVOFFSET
         * round-trips (QEMU's emulation may not). */
        if (vc4_set_voffset(xsd, height) && vc4_set_voffset(xsd, 0))
        {
            /* Clear the back page so the first flip can't expose stale
             * memory. CPU memset to the uncached FB — DMA fill hangs on HW. */
            memset((APTR)(IPTR)xsd->vcsd_FBPage[1], 0, fb_pitch * height);
            xsd->vcsd_FBPages = 2;
        }
        else
            vc4_set_voffset(xsd, 0);
    }

    D(bug("[VideoCoreGfx] %s: %d page(s) @ 0x%p, pitch %d\n",
        __PRETTY_FUNCTION__, (int)xsd->vcsd_FBPages, fb_ptr, (int)fb_pitch));

#ifdef __EMU68__
    /*
     * Say where the scanout is now.
     *
     * The firmware can hand back the same memory at a new depth, so anything
     * that was drawing here before is now drawing at the wrong geometry and
     * has no way to know. This states the fact and names nobody: what reads
     * it is not this driver's business, and a machine with nothing listening
     * loses one OpenResource().
     *
     * A direct call is not merely the wrong shape, it does not link: this
     * driver is installed from DEVS:Drivers and loaded at runtime, so the
     * kernel's symbols are not available to it. The resource is.
     */
    {
        struct ScanoutResource *scanout =
            (struct ScanoutResource *)OpenResource(SCANOUT_RESOURCE_NAME);

        if (scanout && scanout->publish)
            scanout->publish(fb_ptr, fb_pitch, aligned_width, height,
                             bitsperpixel);
    }
#endif

    /* Phase-1 HVS bring-up: dump the firmware's live display list and PV
     * timing for this mode (read-only, self-skips on QEMU). */
    vc4_hvs_dump(xsd, (ULONG)(IPTR)fb_ptr, fb_pitch, aligned_width, height);

    /* Phase 2: own the display list from here on. Flips and cursor
     * updates become dlist repoints; falls back to the firmware paths
     * (and returns FALSE) if the live list can't be inherited. */
    vc4_hvs_takeover(xsd, (ULONG)(IPTR)fb_ptr, fb_pitch);

    *fb_ptr_out   = fb_ptr;
    *fb_pitch_out = fb_pitch;
    return TRUE;
}

/* The firmware drops cursor info on FBALLOC. After a mode switch, push
 * the saved shape and visibility state back so the cursor survives.
 */
static VOID vc4_restore_cursor(struct VideoCoreGfx_staticdata *xsd)
{
    if (!xsd->vcsd_CurBuf || !xsd->vcsd_CurWidth || !xsd->vcsd_CurHeight)
        return;

    /* Owning the display list: the takeover already baked the current
     * cursor state into our list, and the firmware cursor tags would
     * make the firmware rebuild its list and steal the channel back. */
    if (xsd->vcsd_HVS.hvs_Active)
        return;

    VC4_MBOX_LOCK(xsd);
    xsd->vcsd_MBoxMessage[0] = AROS_LE2LONG(12 * 4);
    xsd->vcsd_MBoxMessage[1] = AROS_LE2LONG(VCTAG_REQ);
    xsd->vcsd_MBoxMessage[2] = AROS_LE2LONG(VCTAG_SETCURSORINFO);
    xsd->vcsd_MBoxMessage[3] = AROS_LE2LONG(24);
    xsd->vcsd_MBoxMessage[4] = AROS_LE2LONG(24);
    xsd->vcsd_MBoxMessage[5] = AROS_LE2LONG(xsd->vcsd_CurWidth);
    xsd->vcsd_MBoxMessage[6] = AROS_LE2LONG(xsd->vcsd_CurHeight);
    xsd->vcsd_MBoxMessage[7] = AROS_LE2LONG(0);
    xsd->vcsd_MBoxMessage[8] = AROS_LE2LONG(xsd->vcsd_CurBufBus);
    xsd->vcsd_MBoxMessage[9] = AROS_LE2LONG(xsd->vcsd_CurHotX);
    xsd->vcsd_MBoxMessage[10] = AROS_LE2LONG(xsd->vcsd_CurHotY);
    xsd->vcsd_MBoxMessage[11] = 0;
    MBoxCall((void *)VCMB_BASE, VCMB_PROPCHAN, xsd->vcsd_MBoxMessage);

    xsd->vcsd_MBoxMessage[0] = AROS_LE2LONG(8 * 4);
    xsd->vcsd_MBoxMessage[1] = AROS_LE2LONG(VCTAG_REQ);
    xsd->vcsd_MBoxMessage[2] = AROS_LE2LONG(VCTAG_SETCURSORSTATE);
    xsd->vcsd_MBoxMessage[3] = AROS_LE2LONG(16);
    xsd->vcsd_MBoxMessage[4] = AROS_LE2LONG(16);
    xsd->vcsd_MBoxMessage[5] = AROS_LE2LONG(xsd->vcsd_CurVisible ? 1 : 0);
    xsd->vcsd_MBoxMessage[6] = AROS_LE2LONG((ULONG)xsd->vcsd_CurX);
    xsd->vcsd_MBoxMessage[7] = AROS_LE2LONG((ULONG)xsd->vcsd_CurY);
    xsd->vcsd_MBoxMessage[8] = AROS_LE2LONG(0);
    xsd->vcsd_MBoxMessage[9] = 0;
    MBoxCall((void *)VCMB_BASE, VCMB_PROPCHAN, xsd->vcsd_MBoxMessage);
    VC4_MBOX_UNLOCK(xsd);
}

/*********** BitMap::New() *************************************/

OOP_Object *MNAME_ROOT(New)(OOP_Class *cl, OOP_Object *o, struct pRoot_New *msg)
{
    struct VideoCoreGfx_staticdata *xsd = XSD(cl);
    OOP_Object *pf;
    IPTR width, height, depth;
    IPTR bitsperpixel = 0, bytesperpix_attr = 0;
    ULONG aligned_width, fb_pitch = 0;
    APTR fb_ptr = NULL;
    UBYTE bytesperpix;
    HIDDT_ModeID modeid;

    D(bug("[VideoCoreGfx] VideoCoreGfx.OnBitMap::New()\n"));

    /* Extract the attrs we need for FBALLOC before super-call: ChunkyBM/
     * BitMap cache BytesPerRow at New time, so doing FBALLOC + GETPITCH
     * later and SetAttrs'ing would leave a stale stride.
     *
     * The FB bitmap arrives with just FrameBuffer=TRUE + ModeID;
     * Width/Height/PixFmt are normally derived from the mode database
     * inside BitMap::New, so we replicate that lookup here.
     */
    modeid = (HIDDT_ModeID)GetTagData(aHidd_BitMap_ModeID, vHidd_ModeID_Invalid, msg->attrList);
    width  = GetTagData(aHidd_BitMap_Width,  0, msg->attrList);
    height = GetTagData(aHidd_BitMap_Height, 0, msg->attrList);
    pf     = (OOP_Object *)GetTagData(aHidd_BitMap_PixFmt, 0, msg->attrList);

    if (modeid == vHidd_ModeID_Invalid)
    {
        D(bug("[VideoCoreGfx] OnBitMap::New: no ModeID\n"));
        return NULL;
    }

    if (!width || !height || !pf)
    {
        OOP_Object *sync = NULL;
        OOP_Object *modepf = NULL;

        if (!HIDD_DMEnum_GetMode(xsd->vcsd_DMEnum, modeid, &sync, &modepf)
            || !sync || !modepf)
        {
            D(bug("[VideoCoreGfx] OnBitMap::New: GetMode(0x%08x) failed\n", (ULONG)modeid));
            return NULL;
        }
        if (!width)
            OOP_GetAttr(sync, aHidd_Sync_HDisp, &width);
        if (!height)
            OOP_GetAttr(sync, aHidd_Sync_VDisp, &height);
        if (!pf)
            pf = modepf;
    }

    OOP_GetAttr(pf, aHidd_PixFmt_Depth, &depth);
    OOP_GetAttr(pf, aHidd_PixFmt_BitsPerPixel, &bitsperpixel);
    OOP_GetAttr(pf, aHidd_PixFmt_BytesPerPixel, &bytesperpix_attr);
    if (!width || !height || !bytesperpix_attr)
    {
        D(bug("[VideoCoreGfx] OnBitMap::New: bad geometry %dx%d bytespp %d\n",
            (int)width, (int)height, (int)bytesperpix_attr));
        return NULL;
    }

    /* SETDEPTH wants the cell size in bits. AROS pixfmts that pad colour
     * into wider cells (e.g. BGR032: 24-bit in a 4-byte cell) report
     * BitsPerPixel as colour depth, so use bytesperpix * 8 (the cell width).
     */
    aligned_width = (width + 15) & ~15;
    bytesperpix = (UBYTE)bytesperpix_attr;
    bitsperpixel = (IPTR)bytesperpix * 8;

#if !defined(DEBUGDISPLAY)
    /* Detach the bootloader's FB console: 0x03 to RawPutChar drops
     * ARMI_PutChar (arch/arm-native/kernel/kernel_debug.c) so debug output
     * goes to serial only. Otherwise it keeps drawing into the FB we hand
     * to Workbench, overwriting the title bar.
     */
    RawPutChar(0x03);

    if (!vc4_program_fb(xsd, aligned_width, height, bytesperpix,
                        &fb_ptr, &fb_pitch))
    {
        D(bug("[VideoCoreGfx] OnBitMap::New: FBALLOC failed\n"));
        return NULL;
    }

    D(bug("[VideoCoreGfx] OnBitMap::New: %dx%d %dbpp (depth %d) FB @ 0x%p, pitch %d\n",
        (int)aligned_width, (int)height, (int)bitsperpixel, (int)depth,
        fb_ptr, (int)fb_pitch));

#endif

    /* Hand the FB and pitch to super::New so ChunkyBM/BitMap cache the
     * right values up-front.
     */
    {
        struct TagItem extra_tags[] =
        {
            { aHidd_ChunkyBM_Buffer,    (IPTR)fb_ptr           },
            { aHidd_BitMap_BytesPerRow, fb_pitch               },
            { TAG_MORE,                 (IPTR)msg->attrList    }
        };
        struct pRoot_New new_msg;

        new_msg.mID = msg->mID;
        new_msg.attrList = extra_tags;

        o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&new_msg);
    }

    if (o)
    {
        struct BitmapData *data = OOP_INST_DATA(cl, o);

        memset(data, 0, sizeof(struct BitmapData));
        data->VideoData   = (UBYTE *)fb_ptr;
        data->width       = aligned_width;
        data->height      = height;
        data->bytesperrow = fb_pitch;
        data->bpp         = bitsperpixel;
        data->bytesperpix = bytesperpix;
        data->disp        = -1;
        data->data        = &XSD(cl)->data;
        data->dispwidth   = width;

        xsd->vcsd_FBObj   = o;

#ifdef __EMU68__
        if (vcgfx_assemble_offscreen(xsd, o, data))
            bug("[VideoCoreGfx] assembling off-scanout (front page 0x%p,"
                " rendering to 0x%p) so the boot presentation can stay\n",
                (APTR)(IPTR)xsd->vcsd_FBPage[xsd->vcsd_FBFront],
                data->VideoData);
#endif

        D(bug("[VideoCoreGfx] OnBitMap::New: object @ 0x%p, FB @ 0x%p, pitch %d\n",
            o, data->VideoData, (int)fb_pitch));
    }

    ReturnPtr("VideoCoreGfx.OnBitMap::New: Obj", OOP_Object *, o);
}

/**********  Bitmap::Set()  ***************************************/

/* gfx.hidd's Show() switches the FB ModeID via SetAttrs on us. Left to the
 * BitMap superclass alone it updates Width/Height/BytesPerRow but the
 * firmware keeps scanning out at the old geometry — sheared "intertwined"
 * rendering at every non-native mode. So reprogram the firmware first, then
 * forward the new buffer + pitch to super so cached attributes line up.
 *
 * Returns IPTR to match BM__Root__Set; Show() bails (never copying the new
 * bitmap into us) when SetAttrs returns 0.
 */
IPTR MNAME_ROOT(Set)(OOP_Class *cl, OOP_Object *o, struct pRoot_Set *msg)
{
    struct VideoCoreGfx_staticdata *xsd = XSD(cl);
    struct BitmapData *data = OOP_INST_DATA(cl, o);
    HIDDT_ModeID newmodeid = vHidd_ModeID_Invalid;
    BOOL has_modeid = FALSE;
    struct TagItem *tag, *tstate;
    ULONG idx;
    IPTR result;

    D(bug("[VideoCoreGfx] OnBitMap::Set: entered, attrList=0x%p\n", msg->attrList));

    tstate = msg->attrList;
    while ((tag = NextTagItem(&tstate)))
    {
        if (IS_BM_ATTR(tag->ti_Tag, idx) && idx == aoHidd_BitMap_ModeID)
        {
            newmodeid = (HIDDT_ModeID)tag->ti_Data;
            has_modeid = TRUE;
        }
        else if (IS_VideoCoreGfxBM_ATTR(tag->ti_Tag, idx)
                 && idx == aoHidd_VideoCoreGfxBitMap_Flip)
        {
            /* Page-flip request (e.g. from vc4gallium after a full-frame
             * blit to the back page). */
            if (tag->ti_Data)
                vc4_fb_flip(xsd);
            return TRUE;
        }
        else if (IS_VideoCoreGfxBM_ATTR(tag->ti_Tag, idx)
                 && idx == aoHidd_VideoCoreGfxBitMap_Overlay)
        {
            /* Zero-copy overlay plane (vc4gallium windowed GL). Success
             * is reported via a Get on the same attribute. */
            vc4_hvs_overlay(xsd, (const struct vc4gfx_overlay *)tag->ti_Data);
            return TRUE;
        }
    }

    D(bug("[VideoCoreGfx] OnBitMap::Set: has_modeid=%d, newmodeid=0x%08lx\n",
        (int)has_modeid, (unsigned long)newmodeid));

    if (has_modeid && newmodeid != vHidd_ModeID_Invalid)
    {
        OOP_Object *sync = NULL, *pf = NULL;

        if (HIDD_DMEnum_GetMode(xsd->vcsd_DMEnum, newmodeid, &sync, &pf)
            && sync && pf)
        {
            IPTR width = 0, height = 0, bytesperpix_attr = 0;
            ULONG aligned_width, fb_pitch = 0;
            APTR fb_ptr = NULL;
            UBYTE bytesperpix;

            OOP_GetAttr(sync, aHidd_Sync_HDisp,         &width);
            OOP_GetAttr(sync, aHidd_Sync_VDisp,         &height);
            OOP_GetAttr(pf,   aHidd_PixFmt_BytesPerPixel, &bytesperpix_attr);

            if (width && height && bytesperpix_attr)
            {
                aligned_width = (width + 15) & ~15;
                bytesperpix   = (UBYTE)bytesperpix_attr;

                if (vc4_program_fb(xsd, aligned_width, height, bytesperpix,
                                   &fb_ptr, &fb_pitch) && fb_ptr)
                {
                    struct TagItem extra_tags[] =
                    {
                        { aHidd_ChunkyBM_Buffer,    (IPTR)fb_ptr        },
                        { aHidd_BitMap_BytesPerRow, fb_pitch            },
                        { TAG_MORE,                 (IPTR)msg->attrList }
                    };
                    struct pRoot_Set new_msg;

                    new_msg.mID = msg->mID;
                    new_msg.attrList = extra_tags;

                    result = OOP_DoSuperMethod(cl, o, (OOP_Msg)&new_msg);

                    data->VideoData   = (UBYTE *)fb_ptr;
                    data->width       = aligned_width;
                    data->height      = height;
                    data->bytesperrow = fb_pitch;
                    data->bytesperpix = bytesperpix;
                    data->bpp         = bytesperpix * 8;
                    data->dispwidth   = width;

                    D(bug("[VideoCoreGfx] OnBitMap::Set: switched to %dx%d %dbpp FB @ 0x%p, pitch %d (super=%d)\n",
                        (int)aligned_width, (int)height, (int)data->bpp,
                        fb_ptr, (int)fb_pitch, (int)result));

#ifdef __EMU68__
                    /*
                     * The reprogram above put rendering back on the page being
                     * scanned out. This is the path Wanderer's screen takes --
                     * gfx.hidd switches the mode through SetAttrs on us -- so
                     * without redoing it here the desktop assembles in plain
                     * sight and the retarget done in New() is silently undone.
                     */
                    if (vcgfx_assemble_offscreen(xsd, o, data))
                        bug("[VideoCoreGfx] assembling off-scanout (front page"
                            " 0x%p, rendering to 0x%p) so the boot presentation"
                            " can stay\n",
                            (APTR)(IPTR)xsd->vcsd_FBPage[xsd->vcsd_FBFront],
                            data->VideoData);
#endif
                    vc4_restore_cursor(xsd);
                    return result;
                }

                D(bug("[VideoCoreGfx] OnBitMap::Set: vc4_program_fb failed for %dx%d\n",
                    (int)aligned_width, (int)height));
            }
        }
    }

    return OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/**********  Bitmap::Dispose()  ***********************************/

VOID MNAME_ROOT(Dispose)(OOP_Class *cl, OOP_Object *o, OOP_Msg msg)
{
    EnterFunc(bug("VideoCoreGfx.OnBitMap::Dispose()\n"));
    OOP_DoSuperMethod(cl, o, msg);
    ReturnVoid("VideoCoreGfx.OnBitMap::Dispose");
}
