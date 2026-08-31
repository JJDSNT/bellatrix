/*
    Copyright (C) 2026, The AROS Development Team. All rights reserved.

    Desc: The boot presentation, as the rest of the port drives it.
*/

#ifndef BOOTUI_API_H
#define BOOTUI_API_H

#include <stdint.h>

/*
 * These used to live in boot.h, next to the Emu68 handover context, which
 * made the boot presentation part of one bootloader's glue by address even
 * after it stopped being so by dependency. It has its own header now, and
 * bootui.c includes nothing of Emu68's.
 *
 * Callers here report their own progress; they are not driving a splash. If
 * the boot presentation is removed, what they lose is a listener.
 */
void bootui_init(void);
void bootui_add_resource(void);
void bootui_set_stage(uint32_t stage);
void bootui_clock_start(uint32_t now_us);
void bootui_clock_tick(uint32_t now_us);
void bootui_retarget(void *framebuffer, uint32_t pitch,
                     uint32_t width, uint32_t height, uint32_t depth);
void bootui_takeover(void);
void bootui_hold(void);
void bootui_set_release_hook(void (*hook)(void));
void bootui_release_now(const char *why);
void bootui_repaint(void);
int  bootui_holding(void);
int  bootui_take_release(void);

#endif /* BOOTUI_API_H */
