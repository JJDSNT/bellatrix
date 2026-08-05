#define __OOP_NOATTRBASES__

#include <proto/exec.h>
#include <proto/oop.h>
#include <proto/utility.h>
#include <hidd/gfx.h>

#include "emu68gfx_intern.h"

#include LC_LIBDEFS_FILE

OOP_Object *Emu68Gfx__Root__New(OOP_Class *cl, OOP_Object *o,
                                struct pRoot_New *msg)
{
    struct TagItem pixel_tags[] =
    {
        { aHidd_PixFmt_RedShift,       16 },
        { aHidd_PixFmt_GreenShift,     21 },
        { aHidd_PixFmt_BlueShift,      27 },
        { aHidd_PixFmt_AlphaShift,      0 },
        { aHidd_PixFmt_RedMask,    0xf800 },
        { aHidd_PixFmt_GreenMask,  0x07e0 },
        { aHidd_PixFmt_BlueMask,   0x001f },
        { aHidd_PixFmt_AlphaMask,       0 },
        { aHidd_PixFmt_ColorModel, vHidd_ColorModel_TrueColor },
        { aHidd_PixFmt_Depth,          16 },
        { aHidd_PixFmt_BytesPerPixel,   2 },
        { aHidd_PixFmt_BitsPerPixel,   16 },
        { aHidd_PixFmt_StdPixFmt, vHidd_StdPixFmt_RGB16_LE },
        { aHidd_PixFmt_BitMapType, vHidd_BitMapType_Chunky },
        /*
         * m68k is big-endian; the RGB16_LE framebuffer Emu68 hands us is not.
         * Without this, int_map_truecolor() skips the byte swap it does for
         * every other big-endian target's little-endian pixel formats, and
         * every color comes out with its bytes swapped.
         */
        { aHidd_PixFmt_SwapPixelBytes, TRUE },
        { TAG_DONE, 0 }
    };
    struct TagItem sync_tags[] =
    {
        { aHidd_Sync_PixelClock, 0 },
        { aHidd_Sync_HTotal, 0 },
        { aHidd_Sync_HDisp, 0 },
        { aHidd_Sync_VDisp, 0 },
        { aHidd_Sync_HMax, 16384 },
        { aHidd_Sync_VMax, 16384 },
        { aHidd_Sync_Description, (IPTR)"Emu68:%hx%v" },
        { TAG_DONE, 0 }
    };
    struct TagItem mode_tags[] =
    {
        { aHidd_DMEnum_PixFmtTags, (IPTR)pixel_tags },
        { aHidd_DMEnum_SyncTags, (IPTR)sync_tags },
        { TAG_DONE, 0 }
    };
    struct TagItem new_tags[] =
    {
        { aHidd_Name, (IPTR)"emu68gfx.hidd" },
        { aHidd_HardwareName, (IPTR)"Emu68 linear framebuffer" },
        { aHidd_ProducerName, (IPTR)"Emu68" },
        { TAG_MORE, 0 }
    };
    struct pRoot_New new_msg;

    if (XSD(cl)->gfx)
        return NULL;

    sync_tags[0].ti_Data = 60 * XSD(cl)->width * XSD(cl)->height;
    sync_tags[1].ti_Data = XSD(cl)->width;
    sync_tags[2].ti_Data = XSD(cl)->width;
    sync_tags[3].ti_Data = XSD(cl)->height;
    /*
     * HMax/VMax are what intuition clamps a requested screen size against:
     * openworkbench.c bounds the size it asks for by DTAG_DIMS, which comes
     * from these. This driver owns one linear framebuffer, handed over by the
     * firmware at a fixed size -- it cannot raster anything larger, so saying
     * 16384 makes intuition ask OpenScreen() for the 800x600 its built-in
     * ScreenModePrefs default wants and get nothing back. Report the size we
     * actually have and the request is clamped to it instead.
     */
    sync_tags[4].ti_Data = XSD(cl)->width;
    sync_tags[5].ti_Data = XSD(cl)->height;

    new_tags[3].ti_Data = (IPTR)msg->attrList;
    if (!msg->attrList)
        new_tags[3].ti_Tag = TAG_DONE;
    new_msg.mID = msg->mID;
    new_msg.attrList = new_tags;

    o = (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&new_msg);
    if (o)
    {
        struct TagItem display_tags[] =
        {
            { aHidd_Display_GfxHidd, (IPTR)o },
            { aHidd_Display_ModeTags, (IPTR)mode_tags },
            { TAG_DONE, 0 }
        };

        XSD(cl)->display =
            OOP_NewObject(XSD(cl)->displayclass, NULL, display_tags);
        if (!XSD(cl)->display)
        {
            OOP_DisposeObject(o);
            return NULL;
        }

        XSD(cl)->gfx = o;
        OOP_GetAttr(XSD(cl)->display, aHidd_Display_DMEnumerator,
                    (IPTR *)&XSD(cl)->dmenum);
    }
    return o;
}

VOID Emu68Gfx__Root__Get(OOP_Class *cl, OOP_Object *o,
                         struct pRoot_Get *msg)
{
    ULONG idx;

    if (IS_GFX_ATTR(msg->attrID, idx))
    {
        switch (idx)
        {
        case aoHidd_Gfx_DriverName:
            *msg->storage = (IPTR)"Emu68";
            return;
        case aoHidd_Gfx_DisplayDefault:
            *msg->storage = (IPTR)XSD(cl)->display;
            return;
        /*
         * Tells the generic Display class (gfx_displayclass.c) that this is a
         * single, always-mapped linear framebuffer. It then creates that one
         * bitmap itself (see the FrameBuffer tag below), automatically gives
         * every other displayable bitmap the same class as a "friend" of it,
         * and its own Show()/display_FBInstall() re-link the classic BitMap's
         * HIDD object to the framebuffer whenever a screen is shown - which is
         * the part our own hand-rolled Show() was missing.
         */
        case aoHidd_Gfx_FrameBufferType:
            *msg->storage = vHidd_FrameBuffer_Direct;
            return;
        }
    }
    OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}

OOP_Object *Emu68Display__Hidd_Display__CreateObject(
    OOP_Class *cl, OOP_Object *o, struct pHidd_Display_CreateObject *msg)
{
    BOOL fb = GetTagData(aHidd_BitMap_FrameBuffer, FALSE, msg->attrList);
    BOOL displayable = GetTagData(aHidd_BitMap_Displayable, FALSE, msg->attrList);

    if (msg->cl == XSD(cl)->basebm && (fb || displayable))
    {
        /*
         * Always name the class ourselves instead of letting the generic
         * Display class fall back to "inherit the framebuffer bitmap's
         * class" for displayable bitmaps: since we have no hardware cursor,
         * that framebuffer bitmap is wrapped in a CursorFB proxy, and
         * blindly instantiating THAT class via OOP_NewObject() (skipping
         * its own create_cursorfb() constructor) leaves it with no real
         * bitmap to forward to - every attribute Get() on it then hangs.
         */
        struct TagItem tags[4];
        struct pHidd_Display_CreateObject create_msg;
        int n = 0;

        tags[n].ti_Tag = aHidd_BitMap_ClassID;
        tags[n].ti_Data = (IPTR)CLID_Hidd_ChunkyBM;
        n++;

        if (fb)
        {
            /* The one bitmap the Display class creates for itself (see
             * gfx_displayclass.c) - point it at the real linear framebuffer
             * Emu68 handed us instead of letting ChunkyBM allocate its own
             * backing memory. */
            tags[n].ti_Tag = aHidd_ChunkyBM_Buffer;
            tags[n].ti_Data = (IPTR)XSD(cl)->framebuffer;
            n++;
            tags[n].ti_Tag = aHidd_BitMap_BytesPerRow;
            tags[n].ti_Data = XSD(cl)->pitch;
            n++;
        }

        tags[n].ti_Tag = TAG_MORE;
        tags[n].ti_Data = (IPTR)msg->attrList;

        create_msg.mID = msg->mID;
        create_msg.cl = msg->cl;
        create_msg.attrList = tags;
        return (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)&create_msg);
    }
    return (OOP_Object *)OOP_DoSuperMethod(cl, o, (OOP_Msg)msg);
}
