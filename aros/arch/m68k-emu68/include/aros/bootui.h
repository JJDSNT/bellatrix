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

struct BootUIResource
{
    struct Node node;
    void (*set_stage)(ULONG stage);
};

#endif
