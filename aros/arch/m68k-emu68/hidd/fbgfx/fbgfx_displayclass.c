/*
    Copyright (C) 2016-2026, The AROS Development Team. All rights reserved.

    Desc: Display class for FB.
    Lang: English.
*/

#include <aros/asmcall.h>
#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <aros/symbolsets.h>
#include <devices/inputevent.h>
#include <exec/alerts.h>
#include <exec/memory.h>
#include <hardware/custom.h>
#include <hidd/hidd.h>
#include <hidd/gfx.h>
#include <oop/oop.h>
#include <clib/alib_protos.h>
#include <string.h>

#define DEBUG 0
#include <aros/debug.h>

#ifndef __OOP_NOATTRBASES__
#define __OOP_NOATTRBASES__
#endif

#include "fbgfx_hidd.h"
#include "fbgfx_support.h"

#ifdef __EMU68__
/*
 * m68k-emu68 draws a boot splash straight into the framebuffer before
 * graphics.library exists, and has to be told to stop.
 *
 * There is one linear framebuffer and no way for two producers to share it, so
 * the splash keeps ownership until the display is about to be installed and
 * gives it up in the same breath. This is the same hand-off the port's own
 * driver performs -- see the comment above
 * Emu68Display__Hidd_Display__Show() in arch/m68k-emu68/hidd/emu68gfx -- moved
 * here so that swapping drivers does not silently drop it.
 *
 * Guarded rather than made generic on purpose: what a bootstrap has to be told
 * differs per target, and this file's own initFBGfxHW() shows the aarch64
 * equivalent (RawPutChar(0x03), detaching the framebuffer console). A hook
 * abstracting two unrelated one-liners would cost more than it saved.
 */
#include <bootui_api.h>
#include "fbgfx_intern.h"
#include "fbgfx_bitmap.h"

/*
 * Puts the finished desktop up when the splash lets go.
 *
 * Nothing of the desktop has reached the framebuffer while the hold was on, so
 * this is a full-screen copy rather than a rectangle. Called from
 * bootui_set_stage() -- Wanderer's task -- so taking the framebuffer
 * semaphore and walking OOP objects here is legal, which it would not be from
 * the timer that also can end the hold.
 */
static struct FBGfx_staticdata *fbgfx_held_sd;

static void fbgfx_bootui_released(void)
{
    struct FBGfx_staticdata *sd = fbgfx_held_sd;
    struct FBGfxBitMapData *bmdata;

    if (!sd || !sd->visible)
        return;

    bmdata = OOP_INST_DATA(sd->bmclass, sd->visible);
    LOCK_FRAMEBUFFER(sd);
    fbDoRefreshArea(&sd->data, bmdata, 0, 0, bmdata->width, bmdata->height);
    UNLOCK_FRAMEBUFFER(sd);
}
#endif

#include LC_LIBDEFS_FILE

OOP_Object *FBGfxDisplay__Root__New(OOP_Class *cl, OOP_Object *o, struct pRoot_New *msg)
{
    struct TagItem pftags[] =
    {
        {aHidd_PixFmt_RedShift,     0}, /*  0 */
        {aHidd_PixFmt_GreenShift,   0}, /*  1 */
        {aHidd_PixFmt_BlueShift,    0}, /*  2 */
        {aHidd_PixFmt_AlphaShift,   0}, /*  3 */
        {aHidd_PixFmt_RedMask,      0}, /*  4 */
        {aHidd_PixFmt_GreenMask,    0}, /*  5 */
        {aHidd_PixFmt_BlueMask,     0}, /*  6 */
        {aHidd_PixFmt_AlphaMask,    0}, /*  7 */
        {aHidd_PixFmt_ColorModel,   0}, /*  8 */
        {aHidd_PixFmt_Depth,        0}, /*  9 */
        {aHidd_PixFmt_BytesPerPixel,0}, /* 10 */
        {aHidd_PixFmt_BitsPerPixel, 0}, /* 11 */
        {aHidd_PixFmt_StdPixFmt,    vHidd_StdPixFmt_Native}, /* 12 */
        {aHidd_PixFmt_CLUTShift,    0}, /* 13 */
        {aHidd_PixFmt_CLUTMask,     0}, /* 14 */
        {aHidd_PixFmt_BitMapType,   vHidd_BitMapType_Chunky}, /* 15 */
        /*
         * Set for a surface whose pixels are stored in the opposite byte order
         * to the CPU's -- a 5:6:5 halfword on a big-endian target. Without it
         * int_map_truecolor() skips the swap it performs for every other
         * big-endian target's little-endian formats and every colour arrives
         * with its bytes reversed. initFBGfxHW() decides; this only forwards.
         */
        {aHidd_PixFmt_SwapPixelBytes, 0}, /* 16 */
        {TAG_DONE, 0UL }
    };
    struct TagItem sync_mode[] =
    {
        {aHidd_Sync_PixelClock,         0                       },
        {aHidd_Sync_HTotal,             0                       },
        {aHidd_Sync_HDisp,              0                       },
        {aHidd_Sync_VDisp,              0                       },
        {aHidd_Sync_HMax,               16384                   },
        {aHidd_Sync_VMax,               16384                   },
        {aHidd_Sync_Description,        (IPTR)"FB:%hx%v"      },
        {TAG_DONE,                      0UL                     }
    };
    struct TagItem modetags[] =
    {
        {aHidd_DMEnum_PixFmtTags, (IPTR)pftags},
        {aHidd_DMEnum_SyncTags,   (IPTR)sync_mode},
        {TAG_DONE, 0UL}
    };
    struct TagItem dispTags[] =
    {
        {aHidd_Display_ModeTags, (IPTR)modetags},
        {TAG_MORE, 0UL}
    };
    struct pRoot_New newdispMsg;

    D(bug("[FBGfx:Display] %s()\n", __func__));

    /* Do not allow more than one object instance to be created */
    if (XSD(cl)->vcfbdisplay)
        return NULL;

    pftags[0].ti_Data = XSD(cl)->data.redshift;
    pftags[1].ti_Data = XSD(cl)->data.greenshift;
    pftags[2].ti_Data = XSD(cl)->data.blueshift;
    pftags[4].ti_Data = XSD(cl)->data.redmask;
    pftags[5].ti_Data = XSD(cl)->data.greenmask;
    pftags[6].ti_Data = XSD(cl)->data.bluemask;
    pftags[8].ti_Data = (XSD(cl)->data.depth > 8) ? vHidd_ColorModel_TrueColor : vHidd_ColorModel_Palette;
    pftags[9].ti_Data = (XSD(cl)->data.depth > 24) ? 24 : XSD(cl)->data.depth;
    pftags[10].ti_Data = XSD(cl)->data.bytesperpixel;
    pftags[11].ti_Data = (XSD(cl)->data.bitsperpixel > 24) ? 24 : XSD(cl)->data.bitsperpixel;
    pftags[14].ti_Data = (1 << XSD(cl)->data.depth) - 1;
    pftags[16].ti_Data = XSD(cl)->data.swappixelbytes;

    sync_mode[0].ti_Data = 60 * XSD(cl)->data.width * XSD(cl)->data.height;
    sync_mode[1].ti_Data = XSD(cl)->data.width;
    sync_mode[2].ti_Data = XSD(cl)->data.width;
    sync_mode[3].ti_Data = XSD(cl)->data.height;

    if ((dispTags[1].ti_Data = (IPTR)msg->attrList) == 0)
        dispTags[1].ti_Tag = TAG_DONE;

    newdispMsg.mID = msg->mID;
    newdispMsg.attrList = dispTags;

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&newdispMsg);

    D(bug("[FBGfx:Display] %s: obj @ 0x%p\n", __func__, o));
    return o;
}

VOID FBGfxDisplay__Root__Get(OOP_Class *cl, OOP_Object *o, struct pRoot_Get *msg)
{
    ULONG idx;

    Hidd_Switch (msg->attrID, idx)
    {
    case aoHidd_Name:
        *msg->storage = (IPTR)"FB Display";
        return;
    }

    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

/*********  Display::CreateObject()  ***************************/

OOP_Object *FBGfxDisplay__Hidd_Display__CreateObject(OOP_Class *cl, OOP_Object *o, struct pHidd_Display_CreateObject *msg)
{
    OOP_Object      *object = NULL;

    D(bug("[FBGfx:Display] %s()\n", __func__));
    D(bug("[FBGfx:Display] %s: requested class 0x%p\n", __func__, msg->cl));
    D(bug("[FBGfx:Display] %s: base bitmap class 0x%p\n", __func__, XSD(cl)->basebm));

    if (msg->cl == XSD(cl)->basebm)
    {
        BOOL displayable;
        struct TagItem tags[2] =
        {
            {TAG_IGNORE, 0                  },
            {TAG_MORE  , (IPTR)msg->attrList}
        };
        struct pHidd_Display_CreateObject p;

        displayable = GetTagData(aHidd_BitMap_Displayable, FALSE, msg->attrList);
        if (displayable)
        {
            /* Only displayable bitmaps are bitmaps of our class */
            tags[0].ti_Tag  = aHidd_BitMap_ClassPtr;
            tags[0].ti_Data = (IPTR)XSD(cl)->bmclass;
        }
        else
        {
            /* Non-displayable friends of our bitmaps are plain chunky bitmaps */
            OOP_Object *friend = (OOP_Object *)GetTagData(aHidd_BitMap_Friend, 0, msg->attrList);

            if (friend && (OOP_OCLASS(friend) == XSD(cl)->bmclass))
            {
                tags[0].ti_Tag  = aHidd_BitMap_ClassID;
                tags[0].ti_Data = (IPTR)CLID_Hidd_ChunkyBM;
            }
        }

        p.mID = msg->mID;
        p.cl = msg->cl;
        p.attrList = tags;

        object = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&p);
    }
    else
        object = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);

    ReturnPtr("FBGfx.Display::CreateObject", OOP_Object *, object);
}

/*********  Display::Show()  ***************************/

OOP_Object *FBGfxDisplay__Hidd_Display__Show(OOP_Class *cl, OOP_Object *o, struct pHidd_Display_Show *msg)
{
    struct FBGfx_staticdata *data = XSD(cl);
    struct TagItem tags[] = {
        {aHidd_BitMap_Visible, FALSE},
        {TAG_DONE            , 0    }
    };

    D(bug("[FBGfx:Display] Show(0x%p), old visible 0x%p\n", msg->bitMap, data->visible));

#ifdef __EMU68__
    /*
     * Do not take the framebuffer here -- offer to hold it.
     *
     * Taking it at the first Show() is correct for a driver that composes
     * straight into the framebuffer, and this one does not: it refreshes from
     * the bitmap's own buffer. So a boot presentation can stay up while the
     * desktop is built behind it, and the hand-off becomes one repaint of a
     * finished screen instead of several seconds of half-drawn Workbench.
     *
     * What is registered here is the means, not the policy. Whether anything
     * is held, and for how long, is decided on the other side, from the
     * presentation boundary graphics.library reports above every driver -- a
     * driver deciding a boot lifecycle is the dependency this arrangement
     * exists to avoid. All this says is "if you want to hold, here is how to
     * put the result up when you are done".
     */
    if (msg->bitMap)
    {
        fbgfx_held_sd = data;
        bootui_set_release_hook(fbgfx_bootui_released);
    }
#endif

    LOCK_FRAMEBUFFER(data);

    /* Remove old bitmap from the screen */
    if (data->visible)
    {
        D(bug("[FBGfx:Display] Hiding old bitmap\n"));
        OOP_SetAttrs(data->visible, tags);
    }

    if (msg->bitMap)
    {
        /* If we have a bitmap to show, set it as visible */
        D(bug("[FBGfx:Display] Showing new bitmap\n"));
        tags[0].ti_Data = TRUE;
        OOP_SetAttrs(msg->bitMap, tags);
    }
    else
    {
        D(bug("[FBGfx:Display] Blanking screen\n"));
        /* Otherwise simply clear the framebuffer */
        ClearBuffer(&data->data);
    }

    data->visible = msg->bitMap;
    UNLOCK_FRAMEBUFFER(data);

    D(bug("[FBGfx:Display] Show() done\n"));
    return msg->bitMap;
}
