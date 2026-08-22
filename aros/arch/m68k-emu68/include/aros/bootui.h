#ifndef AROS_EMU68_BOOTUI_H
#define AROS_EMU68_BOOTUI_H

#include <exec/nodes.h>
#include <exec/types.h>

#define BOOTUI_RESOURCE_NAME "bootui.resource"

#define BOOTUI_STAGE_DOS_READY 0x45303231UL
#define BOOTUI_STAGE_STARTUP   0x45303232UL
#define BOOTUI_STAGE_DESKTOP   0x45303233UL
/*
 * The desktop is drawn and the splash may go. Sent after Wanderer has icons on
 * screen, which nothing in the Startup-Sequence can say -- it blocks on
 * WANDERER:Wanderer, so a line after that runs when Wanderer *exits*. Until
 * something is in a position to send it, the hold expires on its own; see
 * emu68_bootui_hold().
 */
#define BOOTUI_STAGE_ICONS     0x45303234UL
/*
 * Copy the screen across again. Sent a moment after the release, because the
 * release copies the bitmap as it stands and that caught the screen title bar
 * one character into its text. Goes through the resource like everything else:
 * the sender is a Zune class, a separate binary on the card, and cannot call
 * into the kernel's symbols.
 */
#define BOOTUI_STAGE_REPAINT   0x45303235UL
/*
 * AROS put something on the display.
 *
 * Reported by graphics.library from display_LoadViewPorts(), which is the one
 * place above every driver where a bitmap becomes the frontmost one -- it
 * covers ShowViewPorts, the software compositor and a plain Show() alike. It
 * is a statement of fact, not an instruction: the sender says what it did and
 * this side decides what that means for the splash.
 *
 * A hook in the generic Display class's Show() would not do. fbgfx overrides
 * Show() and never calls OOP_DoSuperMethod(), so its presentations never reach
 * the base class, while vcgfx does not override Show() at all and always does.
 * Two drivers on the same machine, on opposite sides of that behaviour, is
 * exactly why the boundary belongs above both.
 */
#define BOOTUI_STAGE_PRESENTED 0x45303236UL

struct BootUIResource
{
    struct Node node;
    void (*set_stage)(ULONG stage);
};

/* Move the live early-boot UI to a framebuffer allocated by the display
 * driver. The BootUI accepts the two scanout formats used by this port:
 * little-endian RGB565 and byte-addressed BGRX8888. */
void emu68_bootui_retarget(APTR framebuffer, ULONG pitch,
                           ULONG width, ULONG height, ULONG depth);

#endif
