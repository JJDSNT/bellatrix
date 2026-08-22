/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: What the boot presentation needs from the machine under it.
*/

#ifndef BOOTUI_PLATFORM_H
#define BOOTUI_PLATFORM_H

#include <stdint.h>

/*
 * The boot UI draws a splash before AROS exists, which is the one thing it
 * cannot do through AROS. It needs a surface, a way to say something, and a
 * clock -- and it must not care where any of the three come from, because the
 * moment it knows it is talking to a particular bootloader it stops being
 * AROS-side code and becomes that bootloader's splash.
 *
 * So it asks for them. Whoever brought the machine up answers; on this port
 * that is arch/m68k-emu68/boot, from the handover context before
 * kernel.resource exists and from KrnGetSystemAttr(KATTR_FrameBuffer*) after.
 * Neither fact reaches the other side of this header.
 */

/* Fills in the surface to draw on. Returns 0 when there is none, in which
 * case the boot UI stays quiet and the boot is unaffected. */
int bootui_platform_surface(void **framebuffer, uint32_t *pitch,
                            uint32_t *width, uint32_t *height,
                            uint32_t *depth);

/* One line to whatever the machine uses for early diagnostics. */
void bootui_platform_log(const char *text);

/* The boot arguments, as one string, or NULL. Not parsed here. */
const char *bootui_platform_args(uint32_t *length);

#endif /* BOOTUI_PLATFORM_H */
