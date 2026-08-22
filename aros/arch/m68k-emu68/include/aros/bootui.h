#ifndef AROS_EMU68_BOOTUI_H
#define AROS_EMU68_BOOTUI_H

#include <exec/nodes.h>
#include <exec/types.h>

#define BOOTUI_RESOURCE_NAME "bootui.resource"

/*
 * The milestones the boot presentation understands.
 *
 * This is its own vocabulary, not any bootloader's. Whoever is bringing the
 * machine up translates its own progress into these -- see emu68_set_stage()
 * in arch/m68k-emu68/boot/boot.c -- so the two can diverge without either
 * side learning about the other. The numbers are shared with nothing.
 */
#define BOOTUI_STAGE_ENTRY     0x42553031UL
#define BOOTUI_STAGE_EXEC      0x42553032UL
#define BOOTUI_STAGE_SYSTEM    0x42553033UL
#define BOOTUI_STAGE_KERNEL    0x42553034UL
#define BOOTUI_STAGE_COLDSTART 0x42553035UL
#define BOOTUI_STAGE_GRAPHICS  0x42553036UL

#define BOOTUI_STAGE_DOS_READY 0x45303231UL
#define BOOTUI_STAGE_STARTUP   0x45303232UL
#define BOOTUI_STAGE_DESKTOP   0x45303233UL
/*
 * The desktop is drawn and the splash may go. Sent after Wanderer has icons on
 * screen, which nothing in the Startup-Sequence can say -- it blocks on
 * WANDERER:Wanderer, so a line after that runs when Wanderer *exits*. Until
 * something is in a position to send it, the hold expires on its own; see
 * bootui_hold().
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
/*
 * The boot display has been handed over.
 *
 * Reported by graphics.library when a native driver claims the hardware a
 * DDRV_BootMode driver was serving, which is the moment the surface the boot
 * presentation has been drawing into stops being its to draw into: the
 * incoming driver programs its own mode, and the framebuffer may move, change
 * pitch or change depth without anything asking.
 *
 * It is a display-ownership fact, not a request. That it happens to end a
 * splash is this side's business; no driver reports it, and none needs to
 * know a splash exists. A boot with no native driver never sends it, and
 * there the boot presentation keeps the surface until the desktop is ready,
 * which is exactly right -- nothing took it away.
 */
#define BOOTUI_STAGE_HANDOVER  0x45303237UL

struct BootUIResource
{
    struct Node node;
    void (*set_stage)(ULONG stage);
};

/* Move the live early-boot UI to a framebuffer allocated by the display
 * driver. The BootUI accepts the two scanout formats used by this port:
 * little-endian RGB565 and byte-addressed BGRX8888. */
void bootui_retarget(APTR framebuffer, ULONG pitch,
                           ULONG width, ULONG height, ULONG depth);

#endif
