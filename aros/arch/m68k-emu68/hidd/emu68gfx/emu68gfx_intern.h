#ifndef EMU68GFX_INTERN_H
#define EMU68GFX_INTERN_H

#include <exec/libraries.h>
#include <hidd/gfx.h>
#include <oop/oop.h>

#include "boot.h"

#define ATTRBASES_NUM 8

#define CLID_Hidd_Gfx_Emu68 "hidd.gfx.emu68"
#define CLID_Hidd_Display_Emu68 "hidd.display.emu68"

struct Emu68GfxData
{
};

struct Emu68DisplayData
{
};

struct Emu68GfxStaticData
{
    OOP_Class *basebm;
    OOP_Class *gfxclass;
    OOP_Class *displayclass;
    OOP_Object *gfx;
    OOP_Object *display;
    OOP_Object *dmenum;
    OOP_AttrBase attrBases[ATTRBASES_NUM];
    UBYTE *framebuffer;
    ULONG pitch;
    ULONG width;
    ULONG height;
};

struct Emu68GfxBase
{
    struct Library library;
    struct Emu68GfxStaticData vsd;
};

#define XSD(cl) (&((struct Emu68GfxBase *)cl->UserData)->vsd)

#undef HiddChunkyBMAttrBase
#undef HiddBitMapAttrBase
#undef HiddGfxAttrBase
#undef HiddPixFmtAttrBase
#undef HiddSyncAttrBase
#undef HiddAttrBase
#undef HiddDisplayAttrBase
#undef HiddDMEnumAttrBase

#define HiddChunkyBMAttrBase XSD(cl)->attrBases[0]
#define HiddBitMapAttrBase   XSD(cl)->attrBases[1]
#define HiddGfxAttrBase      XSD(cl)->attrBases[2]
#define HiddPixFmtAttrBase   XSD(cl)->attrBases[3]
#define HiddSyncAttrBase     XSD(cl)->attrBases[4]
#define HiddAttrBase         XSD(cl)->attrBases[5]
#define HiddDisplayAttrBase  XSD(cl)->attrBases[6]
#define HiddDMEnumAttrBase   XSD(cl)->attrBases[7]

#endif
