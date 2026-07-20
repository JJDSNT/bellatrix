// src/launcher/launcher_ui.h
// Shared framebuffer UI toolkit for the Bellatrix launcher screens.
//
// This is infrastructure, not a flow: the media selector (media_selection.c)
// and the Bluetooth scan screen (btscan.c) both draw with these primitives so
// neither owns the font, palette or layout metrics.  launcher.c initialises
// the metrics once and coordinates the screens.
#pragma once
#include <stdint.h>
#include <stdbool.h>

int kprintf(const char *fmt, ...);

// Framebuffer provided by the platform (VC4 mailbox init).
extern uint16_t *framebuffer;
extern uint32_t  pitch;
extern uint32_t  fb_width;
extern uint32_t  fb_height;

// RGB565 colour palette (little-endian as stored in the framebuffer).
#define COL_BG          0x0001u   // very dark blue
#define COL_TITLE_BG    0x9C00u   // dark amber/brown
#define COL_STATUS_BG   0x2000u   // dark green
#define COL_CURSOR_BG   0xE0FFu   // bright yellow
#define COL_TEXT        0xFFFFu   // white
#define COL_TEXT_SEL    0x0000u   // black (for highlighted row)
#define COL_HINT        0xEF7Bu   // light grey

// Layout metrics derived from the framebuffer size by ui_init_metrics().
// Read by the screen modules; set once at launcher start.
extern uint32_t lui_stride;       // pixels per row (derived from pitch)
extern uint32_t lui_scale;        // integer glyph magnification (1..3)
extern uint32_t lui_char;         // glyph cell size in pixels (8 * lui_scale)
extern uint32_t lui_margin_x;     // left margin of the list
extern uint32_t lui_title_h;      // title bar height
extern uint32_t lui_row_h;        // list row height
extern uint32_t lui_status_h;     // status bar height
extern uint32_t lui_visible_rows; // entries visible on screen at once

void ui_init_metrics(void);

// Drawing primitives. fb_fill_rect drains the BT UART FIFO between scanlines
// when BTStack is built in, so slow uncached MMIO fills cannot overrun the
// 16-byte PL011 RX FIFO and desync the H4 parser.
void     fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint16_t colour);
void     fb_putchar(uint32_t px, uint32_t py, char c, uint16_t fg, uint16_t bg);
uint32_t fb_puts(uint32_t x, uint32_t y, const char *s, uint16_t fg, uint16_t bg);
void     fb_puts_centred(uint32_t x0, uint32_t x1, uint32_t y, const char *s,
                         uint16_t fg, uint16_t bg);
void     draw_message(const char *msg, uint16_t bg_col);

// Boot-phase glue shared by the coordinator and both screens.
void     wait_ack(void);              // bounded "press any key / else continue"
uint32_t launcher_ms_since(uint64_t t0_ticks); // wall-clock ms since a snapshot
