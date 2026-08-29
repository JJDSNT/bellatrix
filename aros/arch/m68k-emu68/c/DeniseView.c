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

#include <intuition/intuition.h>
#include <cybergraphx/cybergraphics.h>
#include <dos/dos.h>

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

int main(void)
{
    struct Window *window;
    struct RDArgs *args;
    IPTR arg[1] = { 0 };
    ULONG pitch, width, height, flags;

    args = ReadArgs("NOWINDOW/S", arg, NULL);

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
