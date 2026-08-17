/*
 * Debug console for the native Emu68 boot path.
 *
 * This is Emu68's answer to requirement 2 of <aros/bootcontract.h>: an address
 * that absorbs a byte store. Emu68 leaves 0xdeadbeef unmapped and turns the
 * fault into its own host-side kprintf(), which reaches the real UART.
 *
 * This file is the one place that address appears. The kernel half reaches it
 * through m68k_boot_putc, installed below, so a different machine supplies a
 * different sink and changes nothing above.
 *
 * Progress messages go out this way instead of being drawn on the
 * framebuffer, so they no longer collide with whatever
 * graphics.library/Intuition puts on the same physical screen.
 */

#include "boot.h"

int emu68_console_putc(int chr)
{
    *(volatile uint8_t *)0xdeadbeef = (uint8_t)chr;
    return chr;
}

void emu68_console_init(void *framebuffer, uint32_t pitch,
                        uint32_t width, uint32_t height)
{
    (void)framebuffer;
    (void)pitch;
    (void)width;
    (void)height;

    m68k_boot_putc = emu68_console_putc;
}

void emu68_console_puts(const char *text)
{
    while (*text)
        emu68_console_putc(*text++);
}

