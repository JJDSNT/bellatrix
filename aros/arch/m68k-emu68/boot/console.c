/*
 * Debug console for the native Emu68 boot path.
 *
 * Progress messages go out over the 0xdeadbeef host debug channel (see
 * kernel_debug.c) instead of being drawn on the framebuffer, so they no
 * longer collide with whatever graphics.library/Intuition puts on the same
 * physical screen.
 */

#include "boot.h"

void emu68_console_init(void *framebuffer, uint32_t pitch,
                        uint32_t width, uint32_t height)
{
    (void)framebuffer;
    (void)pitch;
    (void)width;
    (void)height;
}

int emu68_console_putc(int chr)
{
    *(volatile uint8_t *)0xdeadbeef = (uint8_t)chr;
    return 1;
}

void emu68_console_puts(const char *text)
{
    while (*text)
        emu68_console_putc(*text++);
}

