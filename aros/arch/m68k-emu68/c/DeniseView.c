/*
 * DeniseView -- show what the classic chipset drew.
 *
 * Bellatrix publishes each finished Denise frame into the guest's address
 * space: the pixels at $01000000 and a descriptor at $01200000 saying where
 * and how big they are (src/amiga/frame.h). This reads both.
 *
 * It always prints the descriptor and takes a census of the pixels from the
 * guest side, because that is the half that works headless and it is the
 * claim worth proving: that the m68k can read what the chipset drew. The
 * window is the other half, and it is skipped with NOWINDOW.
 *
 * This is a viewer, not a display driver. When AROS draws through the chipset
 * for real -- amigavideo, ISSUE-0068 phase 3 -- the display driver is that,
 * and what puts Denise on the panel is a plane on the video scaler, not this.
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/cybergraphics.h>
#include <proto/oop.h>

#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <dos/dos.h>
#include <hidd/gfx.h>
#include <oop/oop.h>

#define FRAME_BASE      0x01000000UL
#define DESC_BASE       0x01200000UL

#define REG_MAGIC       0x00u
#define REG_VERSION     0x04u
#define REG_BASE        0x08u
#define REG_PITCH       0x0cu
#define REG_WIDTH       0x10u
#define REG_HEIGHT      0x14u
#define REG_FLAGS       0x18u
#define REG_COUNT       0x1cu
#define REG_PHYS        0x20u

#define FRAME_MAGIC     0x444e5345UL    /* 'DNSE' */
#define FRAME_VALID     0x00000001UL

static ULONG desc(ULONG offset)
{
    return *(volatile ULONG *)(DESC_BASE + offset);
}

/*
 * The same census Bellatrix takes on its own side, taken here instead. Run on
 * both, the two numbers answer a question neither can answer alone: whether
 * the frame the chipset composed is the frame the guest can see.
 */
static void census(ULONG base, ULONG pitch, ULONG width, ULONG height)
{
    const ULONG stride = 7;
    ULONG background = *(volatile ULONG *)base;
    ULONG different = 0, sampled = 0, sum = 0;
    ULONG x, y;

    for (y = 0; y < height; y += stride)
    {
        volatile ULONG *row = (volatile ULONG *)(base + y * pitch);

        for (x = 0; x < width; x += stride)
        {
            ULONG pixel = row[x];

            sampled++;
            sum = (sum * 31UL) + pixel;
            if (pixel != background)
                different++;
        }
    }

    Printf("DeniseView: bg=%08lx non-bg=%lu/%lu sum=%08lx\n",
           background, different, sampled, sum);
}

/*
 * Put the chipset's frame on the panel, as a plane the video scaler composites.
 *
 * Everything below this line already worked: Bellatrix publishes each finished
 * Denise frame into an aperture and says where it is, and both sides agree on
 * what is in it. What was missing is the last hop -- nothing put it on the
 * screen -- and the consequence was a machine that renders correctly and looks
 * frozen, which is an expensive way to be right.
 *
 * vcgfx already owns a zero-copy overlay plane, built for windowed GL: give it
 * a physical address, a pitch, a source size and an on-screen size and the
 * scaler composites it above the framebuffer. A 320x256 chipset frame scaled
 * onto a 1080p desktop, beside the desktop rather than instead of it, is
 * exactly what that is for.
 *
 * Mirrored by hand from the driver's vcgfx_bitmap.h, which is private to it --
 * the same way vc4gallium mirrors it.
 */
#define IID_Hidd_BitMap_VideoCore4  "hidd.bitmap.bcmvc4"
#define aoHidd_VC4BM_Overlay        3

struct vc4gfx_overlay
{
    ULONG ovl_Phys;
    ULONG ovl_Pitch;
    ULONG ovl_Width, ovl_Height;
    LONG  ovl_X, ovl_Y;
    ULONG ovl_DestW, ovl_DestH;
};

/* proto/oop.h already declares OOPBase; we just fill it in. */

static LONG show_overlay(ULONG phys, ULONG pitch, ULONG width, ULONG height,
                         ULONG destw, ULONG desth)
{
    struct Screen *screen;
    OOP_Object *bm_obj;
    OOP_AttrBase ab;
    struct vc4gfx_overlay desc;
    struct TagItem tags[2];

    OOPBase = OpenLibrary("oop.library", 0);
    if (!OOPBase)
    {
        Printf("DeniseView: no oop.library.\n");
        return RETURN_FAIL;
    }

    ab = OOP_ObtainAttrBase((STRPTR)IID_Hidd_BitMap_VideoCore4);
    if (!ab)
    {
        Printf("DeniseView: this display driver has no overlay plane.\n"
               "            The scaler belongs to vcgfx; if it is not the\n"
               "            display, there is nothing to composite onto.\n");
        CloseLibrary(OOPBase);
        return RETURN_WARN;
    }

    screen = LockPubScreen(NULL);
    if (!screen)
    {
        Printf("DeniseView: no public screen to put it on.\n");
        OOP_ReleaseAttrBase((STRPTR)IID_Hidd_BitMap_VideoCore4);
        CloseLibrary(OOPBase);
        return RETURN_FAIL;
    }

    bm_obj = HIDD_BM_OBJ(screen->RastPort.BitMap);

    desc.ovl_Phys   = phys;
    desc.ovl_Pitch  = pitch;
    desc.ovl_Width  = width;
    desc.ovl_Height = height;
    desc.ovl_X      = 0;
    desc.ovl_Y      = 0;
    desc.ovl_DestW  = destw;
    desc.ovl_DestH  = desth;

    tags[0].ti_Tag  = ab + aoHidd_VC4BM_Overlay;
    tags[0].ti_Data = (IPTR)&desc;
    tags[1].ti_Tag  = TAG_DONE;

    OOP_SetAttrs(bm_obj, tags);

    Printf("DeniseView: plane up -- %lux%lu at $%08lx, scaled to %lux%lu.\n",
           width, height, phys, destw, desth);
    Printf("            It stays while this program runs. Ctrl-C to take it down.\n");

    /* The plane is live for as long as we hold it; taking it down is a NULL. */
    Wait(SIGBREAKF_CTRL_C);

    tags[0].ti_Data = (IPTR)NULL;
    OOP_SetAttrs(bm_obj, tags);
    Printf("DeniseView: plane down.\n");

    UnlockPubScreen(NULL, screen);
    OOP_ReleaseAttrBase((STRPTR)IID_Hidd_BitMap_VideoCore4);
    CloseLibrary(OOPBase);
    return RETURN_OK;
}

int main(void)
{
    struct Window *window;
    struct RDArgs *args;
    IPTR arg[3] = { 0, 0, 0 };
    ULONG pitch, width, height, flags;

    args = ReadArgs("NOWINDOW/S,SHOW/S,SCALE/K/N", arg, NULL);

    if (desc(REG_MAGIC) != FRAME_MAGIC)
    {
        Printf("DeniseView: no frame aperture here (magic %08lx, wanted %08lx).\n"
               "            Is this a CONFIG_RIGEL build?\n",
               desc(REG_MAGIC), (ULONG)FRAME_MAGIC);
        if (args) FreeArgs(args);
        return RETURN_FAIL;
    }

    flags  = desc(REG_FLAGS);
    pitch  = desc(REG_PITCH);
    width  = desc(REG_WIDTH);
    height = desc(REG_HEIGHT);

    Printf("DeniseView: v%lu %lux%lu pitch=%lu at $%08lx, frame %lu%s\n",
           desc(REG_VERSION), width, height, pitch, desc(REG_BASE),
           desc(REG_COUNT),
           (flags & FRAME_VALID) ? "" : " (nothing published yet)");

    if (!(flags & FRAME_VALID) || !width || !height || !pitch)
    {
        Printf("DeniseView: nothing to show. Has anything programmed the "
               "chipset?\n");
        if (args) FreeArgs(args);
        return RETURN_WARN;
    }

    census(FRAME_BASE, pitch, width, height);

    if (arg[1])
    {
        ULONG scale = arg[2] ? (ULONG)*(LONG *)arg[2] : 3;
        LONG rc;

        if (scale < 1) scale = 1;
        rc = show_overlay(desc(REG_PHYS), pitch, width, height,
                          width * scale, height * scale);
        if (args) FreeArgs(args);
        return rc;
    }

    if (arg[0])
    {
        if (args) FreeArgs(args);
        return RETURN_OK;
    }

    if (CyberGfxBase == NULL)
    {
        Printf("DeniseView: no cybergraphics.library, so no window.\n");
        if (args) FreeArgs(args);
        return RETURN_WARN;
    }

    window = OpenWindowTags(NULL,
        WA_Title,        (IPTR)"Denise",
        WA_InnerWidth,   width,
        WA_InnerHeight,  height,
        WA_DragBar,      TRUE,
        WA_DepthGadget,  TRUE,
        WA_CloseGadget,  TRUE,
        WA_SimpleRefresh, TRUE,
        WA_IDCMP,        IDCMP_CLOSEWINDOW,
        TAG_DONE);

    if (window == NULL)
    {
        Printf("DeniseView: could not open a window.\n");
        if (args) FreeArgs(args);
        return RETURN_FAIL;
    }

    for (;;)
    {
        struct IntuiMessage *message;
        BOOL done = FALSE;

        WritePixelArray((APTR)FRAME_BASE, 0, 0, pitch, window->RPort,
                        window->BorderLeft, window->BorderTop,
                        width, height, RECTFMT_RGBA);

        while ((message = (struct IntuiMessage *)GetMsg(window->UserPort)))
        {
            if (message->Class == IDCMP_CLOSEWINDOW)
                done = TRUE;
            ReplyMsg((struct Message *)message);
        }
        if (done)
            break;

        /* The chipset is slow enough that anything faster is wasted work. */
        Delay(2);
    }

    CloseWindow(window);
    if (args) FreeArgs(args);
    return RETURN_OK;
}
