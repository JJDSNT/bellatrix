/*
 * Putting the chipset's picture on the panel, without being asked.
 *
 * Bellatrix composes the classic chipset's frame on its own core and publishes
 * each finished one into an aperture, with a descriptor beside it saying where
 * the frame is and what Denise was doing while it was made. Getting it onto
 * the display is one more hop: the scaler composites an overlay plane above
 * the framebuffer, and something has to raise that plane.
 *
 * Until now nothing did unless a person ran DeniseView by hand, and the result
 * was the worst kind of correct: an Amiga screen that opens, a Copper that
 * runs, a blitter that draws -- and a black panel. The two halves worked and
 * neither was visible.
 *
 * The rule here is the chipset's own output, not an Intuition event. That is
 * deliberate and it is not a shortcut:
 *
 *   - `vc4_hvs_overlay()` takes the mailbox lock, so it cannot be called from
 *     the vsync interrupt, and amigavideo's ShowViewPorts runs under
 *     Intuition's locks, where reaching across to another driver's bitmap
 *     object invites a deadlock. A task of our own owes nothing to either.
 *   - RIGEL_FRAME_COPPER_ACTIVE means the Copper executed at least one MOVE
 *     while that frame was composed, and the Copper executes a MOVE only when
 *     something programmed a display. So it is true exactly while there is an
 *     Amiga picture to show, which is the condition we actually want, arrived
 *     at without asking anyone.
 *
 * "A frame has been published" would not do: it is true from the first VBLANK
 * and stays true forever, so it cannot tell a picture from an idle machine.
 */

#include <aros/debug.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "vcgfx_hidd.h"
#include "vcgfx_hvs.h"
#include "vcgfx_bitmap.h"

/* Mirrors src/amiga/frame.h, which belongs to the Emu68 side of the machine.
 * Duplicated rather than shared because the two sides are built by different
 * toolchains; the descriptor's version field is what keeps them honest. */
#define DENISE_DESC_BASE        0x01200000UL
#define DENISE_REG_MAGIC        0x00u
#define DENISE_REG_VERSION      0x04u
#define DENISE_REG_PITCH        0x0cu
#define DENISE_REG_WIDTH        0x10u
#define DENISE_REG_HEIGHT       0x14u
#define DENISE_REG_FLAGS        0x18u
#define DENISE_REG_COUNT        0x1cu
#define DENISE_REG_PHYS         0x20u
#define DENISE_REG_CHIPFLAGS    0x24u

#define DENISE_MAGIC            0x444e5345UL   /* 'DNSE' */
#define DENISE_MIN_VERSION      3UL
#define DENISE_FLAG_VALID       0x00000001UL
/*
 * Bitplane DMA, not COPPER_ACTIVE.
 *
 * COPPER_ACTIVE was the first rule here and it was wrong. amigavideo's
 * initcustom programs a default Copper list at driver init and leaves it
 * running, so the flag is set from then on with no screen open -- and this
 * task duly raised an empty 256x256 picture over the boot display, which is
 * what a person saw. "The Copper executes a MOVE only when something
 * programmed a display" was simply not true.
 *
 * BPLEN is the bit that means what was wanted: amigavideo's compositor sets it
 * when it shows a screen and clears it when it blanks.
 */
#define DENISE_CHIP_BPLEN       0x80000000UL

/*
 * How the picture is placed.
 *
 * A 320x256 chipset frame on a 1080p panel is unreadable at 1:1, so it is
 * scaled by a whole number -- whole because a classic display has square-ish
 * pixels of its own and a fractional scale turns them into interference. Three
 * fits 256 rows into 768 of 1080 with room to spare.
 */
#define DENISE_SCALE            3

static ULONG denise_reg(ULONG offset)
{
    return *(volatile ULONG *)(DENISE_DESC_BASE + offset);
}

/*
 * Poll, rather than wait on something.
 *
 * There is nothing to wait on: the producer is a core outside this operating
 * system and it signals nothing. Five times a second is far below the cost of
 * noticing, and the thing being noticed -- a screen opening or closing -- is a
 * human-scale event.
 */
#define DENISE_TICK             10      /* Delay() units: 1/50 s each */

static void denise_watcher(void)
{
    struct VideoCoreGfx_staticdata *xsd;
    struct vc4gfx_overlay ovl;
    BOOL up = FALSE;
    ULONG last_count = 0;
    ULONG idle_ticks = 0;

    xsd = (struct VideoCoreGfx_staticdata *)FindTask(NULL)->tc_UserData;

    for (;;)
    {
        ULONG count, chipflags;
        BOOL want;

        Delay(DENISE_TICK);

        if (denise_reg(DENISE_REG_MAGIC) != DENISE_MAGIC ||
            denise_reg(DENISE_REG_VERSION) < DENISE_MIN_VERSION ||
            !(denise_reg(DENISE_REG_FLAGS) & DENISE_FLAG_VALID))
            continue;

        count     = denise_reg(DENISE_REG_COUNT);
        chipflags = denise_reg(DENISE_REG_CHIPFLAGS);

        /*
         * Two conditions, and both are needed.
         *
         * COPPER_ACTIVE says a display is programmed; an advancing frame count
         * says the chipset is still running. A machine that stopped with the
         * Copper flag set from its last frame would otherwise hold a frozen
         * picture over the desktop forever.
         */
        want = (chipflags & DENISE_CHIP_BPLEN) != 0 && count != last_count;

        if (count != last_count)
        {
            last_count = count;
            idle_ticks = 0;
        }
        else if (idle_ticks < 4u)
        {
            /* Tolerate a tick or two with no new frame before taking the
             * picture down: the poll and the producer are not in step. */
            idle_ticks++;
            want = up;
        }

        if (!want && !up)
            continue;

        if (want)
        {
            ovl.ovl_Phys   = denise_reg(DENISE_REG_PHYS);
            ovl.ovl_Pitch  = denise_reg(DENISE_REG_PITCH);
            ovl.ovl_Width  = denise_reg(DENISE_REG_WIDTH);
            ovl.ovl_Height = denise_reg(DENISE_REG_HEIGHT);
            ovl.ovl_X      = 0;
            ovl.ovl_Y      = 0;
            ovl.ovl_DestW  = ovl.ovl_Width * DENISE_SCALE;
            ovl.ovl_DestH  = ovl.ovl_Height * DENISE_SCALE;

            if (ovl.ovl_Phys == 0 || ovl.ovl_Width == 0 || ovl.ovl_Height == 0)
                continue;

            /*
             * Applied on every tick, not only on the way up.
             *
             * vcgfx re-authors the whole display list whenever it takes the
             * display or changes mode, and that drops the plane without
             * telling anyone -- `hvs_takeover` clears hvs_OvlActive outright.
             * A task that raised the plane once and then trusted its own
             * memory of it showed a rectangle during the boot display and
             * nothing afterwards.
             *
             * Re-applying costs nothing in the steady state: unchanged
             * geometry patches the live entry rather than rebuilding the list.
             * After a rebuild the same call is structural again and the plane
             * comes back by itself.
             */
            if (vc4_hvs_overlay(xsd, &ovl))
            {
                if (!up)
                    bug("[VideoCoreGfx:Denise] plane up -- %ux%u at $%08x,"
                        " scaled to %ux%u\n",
                        (unsigned)ovl.ovl_Width, (unsigned)ovl.ovl_Height,
                        (unsigned)ovl.ovl_Phys,
                        (unsigned)ovl.ovl_DestW, (unsigned)ovl.ovl_DestH);
                up = TRUE;
            }
            else
            {
                /*
                 * Say so once rather than retry silently. The overlay is
                 * refused on a scaled desktop -- the plane's contents would
                 * need scaling to line up -- and a person looking at a black
                 * screen deserves to know that is the reason.
                 */
                static BOOL complained;

                if (!complained)
                {
                    complained = TRUE;
                    bug("[VideoCoreGfx:Denise] the scaler refused the plane;"
                        " the desktop is probably not at its native size\n");
                }
            }
        }
        else
        {
            vc4_hvs_overlay(xsd, NULL);
            up = FALSE;
            bug("[VideoCoreGfx:Denise] plane down -- the chipset stopped"
                " composing\n");
        }
    }
}

void vc4_denise_init(struct VideoCoreGfx_staticdata *xsd)
{
    struct Task *task;

    /*
     * Below every driver and below the input handler: this exists to notice
     * something, and noticing must never come at the cost of the machine
     * feeling slower than it is.
     */
    task = NewCreateTask(TASKTAG_NAME,      "Bellatrix Denise plane",
                         TASKTAG_PRI,       -5,
                         TASKTAG_PC,        (IPTR)denise_watcher,
                         TASKTAG_USERDATA,  (IPTR)xsd,
                         TAG_DONE);

    if (task == NULL)
        bug("[VideoCoreGfx:Denise] could not start the watcher;"
            " the chipset's picture will need DeniseView by hand\n");
}
