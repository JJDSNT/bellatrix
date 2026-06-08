// src/launcher/launcher.c
// Media selector UI for Bellatrix bare-metal.
//
// Shows a list of .ADF (floppy) and .ISO (CD-ROM) files found on a USB
// pen drive.  Navigation: UP/DOWN arrows, ENTER to select, ESC to boot
// without disk.
//
// ADF: loaded entirely into a static RAM buffer, inserted via
//      bellatrix_machine_insert_df0_adf().
// ISO: opened via FAT32 and read on-demand sector-by-sector through a
//      callback registered with the lide_cdrom Zorro II expansion.
//
// QEMU: when no USB drive is present the launcher checks for images
//       injected via -device loader (physical 0x18000000/0x20000000).

#include "launcher/launcher.h"
#include "launcher/launcher_input.h"
#include "storage/fat/fat32.h"
#include "storage/iso/iso_image.h"
#if BELLATRIX_ENABLE_USBSTACK
#include "io/usb/usb_msc_bellatrix.h"
#endif

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

int      kprintf(const char *fmt, ...);
int      bellatrix_machine_insert_df0_adf(const uint8_t *adf, uint32_t adf_size);
int      bellatrix_machine_insert_iso(const void *data, size_t size);
int      bellatrix_machine_attach_iso_fn(iso_read_fn fn, void *ctx, uint32_t sector_count);
void     bellatrix_launcher_pump_usb(void);

extern uint16_t *framebuffer;
extern uint32_t  pitch;
extern uint32_t  fb_width;
extern uint32_t  fb_height;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MAX_ADF_FILES   64u
#define MAX_ISO_FILES   32u
#define ADF_BUF_SIZE    (1024u * 1024u)   // 1 MB — large enough for any DD/HD ADF
#define VISIBLE_ROWS    18u               // entries visible on screen at once

// Combined media list entry
typedef enum {
    MEDIA_ADF = 0,
    MEDIA_ISO,
} MediaType;

typedef struct {
    char      name[FAT32_NAME_MAX];
    MediaType type;
} MediaEntry;

// RGB565 colour palette
#define COL_BG          0x0001u   // very dark blue (LE)
#define COL_TITLE_BG    0x9C00u   // dark amber/brown
#define COL_STATUS_BG   0x2000u   // dark green
#define COL_CURSOR_BG   0xE0FFu   // bright yellow  (LE)
#define COL_TEXT        0xFFFFu   // white
#define COL_TEXT_SEL    0x0000u   // black (for highlighted row)
#define COL_HINT        0xEF7Bu   // light grey

// ---------------------------------------------------------------------------
// 8×8 bitmap font — printable ASCII 0x20..0x7E
// Each entry: 8 bytes, one per row, MSB = leftmost pixel
// ---------------------------------------------------------------------------

static const uint8_t s_font[95][8] = {
    /* 0x20 ' '  */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 0x21 '!'  */ { 0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00 },
    /* 0x22 '"'  */ { 0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00 },
    /* 0x23 '#'  */ { 0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00 },
    /* 0x24 '$'  */ { 0x0C,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00 },
    /* 0x25 '%'  */ { 0x62,0x66,0x0C,0x18,0x30,0x66,0x46,0x00 },
    /* 0x26 '&'  */ { 0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00 },
    /* 0x27 '\'' */ { 0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00 },
    /* 0x28 '('  */ { 0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00 },
    /* 0x29 ')'  */ { 0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00 },
    /* 0x2A '*'  */ { 0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00 },
    /* 0x2B '+'  */ { 0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00 },
    /* 0x2C ','  */ { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30 },
    /* 0x2D '-'  */ { 0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00 },
    /* 0x2E '.'  */ { 0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00 },
    /* 0x2F '/'  */ { 0x02,0x06,0x0C,0x18,0x30,0x60,0x40,0x00 },
    /* 0x30 '0'  */ { 0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00 },
    /* 0x31 '1'  */ { 0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00 },
    /* 0x32 '2'  */ { 0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00 },
    /* 0x33 '3'  */ { 0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00 },
    /* 0x34 '4'  */ { 0x0C,0x1C,0x2C,0x4C,0x7E,0x0C,0x0C,0x00 },
    /* 0x35 '5'  */ { 0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00 },
    /* 0x36 '6'  */ { 0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00 },
    /* 0x37 '7'  */ { 0x7E,0x06,0x0C,0x18,0x18,0x18,0x18,0x00 },
    /* 0x38 '8'  */ { 0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00 },
    /* 0x39 '9'  */ { 0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00 },
    /* 0x3A ':'  */ { 0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00 },
    /* 0x3B ';'  */ { 0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00 },
    /* 0x3C '<'  */ { 0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00 },
    /* 0x3D '='  */ { 0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00 },
    /* 0x3E '>'  */ { 0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00 },
    /* 0x3F '?'  */ { 0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00 },
    /* 0x40 '@'  */ { 0x3C,0x66,0x6E,0x6A,0x6E,0x60,0x3C,0x00 },
    /* 0x41 'A'  */ { 0x18,0x3C,0x66,0x7E,0x66,0x66,0x66,0x00 },
    /* 0x42 'B'  */ { 0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00 },
    /* 0x43 'C'  */ { 0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00 },
    /* 0x44 'D'  */ { 0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00 },
    /* 0x45 'E'  */ { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00 },
    /* 0x46 'F'  */ { 0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00 },
    /* 0x47 'G'  */ { 0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0x00 },
    /* 0x48 'H'  */ { 0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00 },
    /* 0x49 'I'  */ { 0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00 },
    /* 0x4A 'J'  */ { 0x1E,0x0C,0x0C,0x0C,0x0C,0x6C,0x38,0x00 },
    /* 0x4B 'K'  */ { 0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00 },
    /* 0x4C 'L'  */ { 0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00 },
    /* 0x4D 'M'  */ { 0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00 },
    /* 0x4E 'N'  */ { 0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00 },
    /* 0x4F 'O'  */ { 0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },
    /* 0x50 'P'  */ { 0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00 },
    /* 0x51 'Q'  */ { 0x3C,0x66,0x66,0x66,0x66,0x3C,0x0E,0x00 },
    /* 0x52 'R'  */ { 0x7C,0x66,0x66,0x7C,0x6C,0x66,0x63,0x00 },
    /* 0x53 'S'  */ { 0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00 },
    /* 0x54 'T'  */ { 0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00 },
    /* 0x55 'U'  */ { 0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00 },
    /* 0x56 'V'  */ { 0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00 },
    /* 0x57 'W'  */ { 0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00 },
    /* 0x58 'X'  */ { 0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00 },
    /* 0x59 'Y'  */ { 0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x00 },
    /* 0x5A 'Z'  */ { 0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00 },
    /* 0x5B '['  */ { 0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00 },
    /* 0x5C '\\' */ { 0x40,0x60,0x30,0x18,0x0C,0x06,0x02,0x00 },
    /* 0x5D ']'  */ { 0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00 },
    /* 0x5E '^'  */ { 0x10,0x38,0x6C,0x00,0x00,0x00,0x00,0x00 },
    /* 0x5F '_'  */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE },
    /* 0x60 '`'  */ { 0x18,0x18,0x0C,0x00,0x00,0x00,0x00,0x00 },
    /* 0x61 'a'  */ { 0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00 },
    /* 0x62 'b'  */ { 0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00 },
    /* 0x63 'c'  */ { 0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00 },
    /* 0x64 'd'  */ { 0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00 },
    /* 0x65 'e'  */ { 0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00 },
    /* 0x66 'f'  */ { 0x0E,0x18,0x18,0x3C,0x18,0x18,0x18,0x00 },
    /* 0x67 'g'  */ { 0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x7C },
    /* 0x68 'h'  */ { 0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00 },
    /* 0x69 'i'  */ { 0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00 },
    /* 0x6A 'j'  */ { 0x06,0x00,0x0E,0x06,0x06,0x66,0x3C,0x00 },
    /* 0x6B 'k'  */ { 0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00 },
    /* 0x6C 'l'  */ { 0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00 },
    /* 0x6D 'm'  */ { 0x00,0x00,0x66,0x7F,0x6B,0x63,0x63,0x00 },
    /* 0x6E 'n'  */ { 0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00 },
    /* 0x6F 'o'  */ { 0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00 },
    /* 0x70 'p'  */ { 0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60 },
    /* 0x71 'q'  */ { 0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06 },
    /* 0x72 'r'  */ { 0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00 },
    /* 0x73 's'  */ { 0x00,0x00,0x3C,0x60,0x3C,0x06,0x7C,0x00 },
    /* 0x74 't'  */ { 0x18,0x18,0x7E,0x18,0x18,0x18,0x0E,0x00 },
    /* 0x75 'u'  */ { 0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00 },
    /* 0x76 'v'  */ { 0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00 },
    /* 0x77 'w'  */ { 0x00,0x00,0x63,0x6B,0x7F,0x77,0x63,0x00 },
    /* 0x78 'x'  */ { 0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00 },
    /* 0x79 'y'  */ { 0x00,0x00,0x66,0x66,0x3E,0x06,0x7C,0x00 },
    /* 0x7A 'z'  */ { 0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00 },
    /* 0x7B '{'  */ { 0x0C,0x18,0x18,0x70,0x18,0x18,0x0C,0x00 },
    /* 0x7C '|'  */ { 0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00 },
    /* 0x7D '}'  */ { 0x30,0x18,0x18,0x0E,0x18,0x18,0x30,0x00 },
    /* 0x7E '~'  */ { 0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00 },
};

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

static uint32_t s_stride;  // pixels per row (derived from pitch)

static void fb_init_stride(void)
{
    s_stride = pitch / 2u;
}

static void fb_fill_rect(uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h,
                         uint16_t colour)
{
    for (uint32_t row = 0u; row < h; row++) {
        uint16_t *line = framebuffer + (y + row) * s_stride + x;
        for (uint32_t col = 0u; col < w; col++) {
            line[col] = colour;
        }
    }
}

// Draw one character at pixel (px, py); foreground/background colours
static void fb_putchar(uint32_t px, uint32_t py,
                       char c, uint16_t fg, uint16_t bg)
{
    unsigned idx = 0u;
    if ((unsigned char)c >= 0x20u && (unsigned char)c <= 0x7Eu) {
        idx = (unsigned char)c - 0x20u;
    }

    const uint8_t *glyph = s_font[idx];

    for (unsigned row = 0u; row < 8u; row++) {
        uint8_t  bits = glyph[row];
        uint16_t *dst  = framebuffer + (py + row) * s_stride + px;
        for (unsigned col = 0u; col < 8u; col++) {
            dst[col] = (bits & (0x80u >> col)) ? fg : bg;
        }
    }
}

// Draw a NUL-terminated string; returns x after last character
static uint32_t fb_puts(uint32_t x, uint32_t y,
                        const char *s,
                        uint16_t fg, uint16_t bg)
{
    while (*s) {
        fb_putchar(x, y, *s, fg, bg);
        x += 8u;
        s++;
    }
    return x;
}

// Draw a string centred in [x0, x1)
static void fb_puts_centred(uint32_t x0, uint32_t x1,
                            uint32_t y,
                            const char *s,
                            uint16_t fg, uint16_t bg)
{
    uint32_t len = 0u;
    for (const char *p = s; *p; p++) len++;
    uint32_t w   = len * 8u;
    uint32_t mid = x0 + (x1 - x0) / 2u;
    uint32_t px  = (w < (x1 - x0)) ? (mid - w / 2u) : x0;

    // Fill background for the full row width
    fb_fill_rect(x0, y, x1 - x0, 8u, bg);
    fb_puts(px, y, s, fg, bg);
}

// ---------------------------------------------------------------------------
// Layout constants (all in pixels)
// ---------------------------------------------------------------------------

#define MARGIN_X    16u
#define TITLE_H     20u
#define ROW_H       12u
#define STATUS_H    14u

// ---------------------------------------------------------------------------
// Screen helpers
// ---------------------------------------------------------------------------

static void draw_frame(uint32_t count, uint32_t cursor, uint32_t scroll,
                       const MediaEntry *entries)
{
    if (!framebuffer) return;

    const uint32_t W = fb_width;
    const uint32_t H = fb_height;

    // Background
    fb_fill_rect(0, 0, W, H, COL_BG);

    // Title bar
    fb_fill_rect(0, 0, W, TITLE_H, COL_TITLE_BG);
    fb_puts_centred(0, W, (TITLE_H - 8u) / 2u,
                    "BELLATRIX LAUNCHER  --  Select a disk (ADF=floppy, ISO=CD-ROM):",
                    COL_TEXT, COL_TITLE_BG);

    // File list
    uint32_t list_y = TITLE_H + 4u;

    for (uint32_t i = 0u; i < VISIBLE_ROWS && (scroll + i) < count; i++) {
        uint32_t         idx      = scroll + i;
        const MediaEntry *e       = &entries[idx];
        bool              selected = (idx == cursor);

        uint16_t bg  = selected ? COL_CURSOR_BG : COL_BG;
        uint16_t fg  = selected ? COL_TEXT_SEL  : COL_TEXT;

        uint32_t row_y = list_y + i * ROW_H;

        fb_fill_rect(0, row_y, W, ROW_H, bg);

        if (selected) {
            fb_putchar(MARGIN_X - 10u, row_y, '>', fg, bg);
        }

        // Type tag
        const char *tag = (e->type == MEDIA_ISO) ? "[ISO] " : "[ADF] ";
        uint32_t    x   = fb_puts(MARGIN_X, row_y, tag, COL_HINT, bg);
        fb_puts(x, row_y, e->name, fg, bg);
    }

    // Status bar
    uint32_t status_y = H - STATUS_H;
    fb_fill_rect(0, status_y, W, STATUS_H, COL_STATUS_BG);

    char hint[80];
    const char *prefix = "Files: ";
    unsigned n_chars = 0u;
    for (const char *p = prefix; *p; p++) hint[n_chars++] = *p;
    unsigned tmp = count;
    if (tmp == 0u) { hint[n_chars++] = '0'; }
    else {
        char dbuf[8]; unsigned nd = 0u;
        while (tmp) { dbuf[nd++] = (char)('0' + tmp % 10u); tmp /= 10u; }
        for (unsigned d = nd; d > 0u; d--) hint[n_chars++] = dbuf[d - 1u];
    }
    const char *suffix = "  |  UP/DOWN: navigate  |  ENTER: select  |  ESC: no disk";
    for (const char *p = suffix; *p; p++) hint[n_chars++] = *p;
    hint[n_chars] = '\0';

    fb_puts_centred(0, W, status_y + (STATUS_H - 8u) / 2u,
                    hint, COL_TEXT, COL_STATUS_BG);
}

static void draw_message(const char *msg, uint16_t bg_col)
{
    if (!framebuffer) return;

    const uint32_t W = fb_width;
    const uint32_t H = fb_height;

    fb_fill_rect(0, 0, W, H, COL_BG);
    fb_fill_rect(0, H / 2u - 12u, W, 24u, bg_col);
    fb_puts_centred(0, W, H / 2u - 4u, msg, COL_TEXT, bg_col);
}

// ---------------------------------------------------------------------------
// ADF buffer
// ---------------------------------------------------------------------------

static uint8_t s_adf_buf[ADF_BUF_SIZE] __attribute__((aligned(512)));

// ---------------------------------------------------------------------------
// FAT32 state + ISO file — kept alive for the duration of emulation so that
// the ATAPI read callback can seek and read sectors on demand.
// ---------------------------------------------------------------------------

#if BELLATRIX_ENABLE_USBSTACK
static Fat32State s_fat32;
static Fat32File  s_iso_file;
#endif

static bool fat32_iso_read_cb(void *ctx, uint32_t lba, uint32_t count, void *dst)
{
    Fat32File *f   = (Fat32File *)ctx;
    uint32_t   off = lba * ISO_SECTOR_SIZE;

    if (!fat32_seek(f, off))
        return false;

    uint32_t want = count * ISO_SECTOR_SIZE;
    uint32_t got  = fat32_read(f, dst, want);
    return got == want;
}

// ---------------------------------------------------------------------------
// USB polling / acknowledgement
// ---------------------------------------------------------------------------

static void pump_usb(void)
{
    bellatrix_launcher_pump_usb();
}

// Wait for a key press, pumping USB each iteration.
// Times out after ~6 seconds so a headless setup still proceeds.
static void wait_ack(void)
{
    for (uint32_t i = 0u; i < 5000u; i++) {
        pump_usb();
        if (launcher_input_pop() != 0u) return;
        for (volatile uint32_t d = 0u; d < 1000000u; d++) asm volatile("nop");
    }
}

// ---------------------------------------------------------------------------
// QEMU fallback: load ADF placed by "-device loader,addr=0x18000000"
//
// When running under QEMU with no SD card, the host tool injects the ADF at
// physical 0x18000000 via the QEMU generic loader device.  Emu68's linear
// kernel map places physical P at virtual (0xffffff9000000000 + P).
//
// On real hardware, random RAM at that address won't have the 'DOS' magic, so
// this function is always a no-op on bare metal.
// ---------------------------------------------------------------------------

// Physical addresses where QEMU loader devices inject media images.
// Bellatrix checks these when the SD card init fails (QEMU has no SD card).
#define QEMU_ADF_PHYS    0x18000000UL
#define QEMU_ADF_KVIRT   ((const uint8_t *)(0xffffff9000000000ULL + QEMU_ADF_PHYS))
#define QEMU_ISO_PHYS    0x20000000UL
#define QEMU_ISO_KVIRT   ((const uint8_t *)(0xffffff9000000000ULL + QEMU_ISO_PHYS))
#define ADF_SIZE_DD      901120u   // 80 tracks × 11 sectors × 512 bytes

static bool try_qemu_loader_adf(void)
{
    const uint8_t *p = QEMU_ADF_KVIRT;

    // Amiga OFS/FFS boot block always starts with 'DOS'
    if (p[0] != 'D' || p[1] != 'O' || p[2] != 'S') return false;

    kprintf("[LAUNCHER] QEMU loader ADF at 0x%08lx: type=0x%02x\n",
            (unsigned long)QEMU_ADF_PHYS, (unsigned)p[3]);

    // Copy into the static ADF buffer so the pointer stays valid after launch
    memcpy(s_adf_buf, p, ADF_SIZE_DD);
    return bellatrix_machine_insert_df0_adf(s_adf_buf, ADF_SIZE_DD) != 0;
}

static bool try_qemu_loader_iso(void)
{
    const uint8_t *p = QEMU_ISO_KVIRT;

    // ISO 9660 PVD at sector 16 (offset 0x8000).
    // Byte 0: descriptor type 0x01, bytes 1-5: "CD001"
    if (p[0x8000] != 0x01u ||
        p[0x8001] != 'C' || p[0x8002] != 'D' ||
        p[0x8003] != '0' || p[0x8004] != '0' || p[0x8005] != '1')
        return false;

    // Volume space size (LE32) at PVD offset 80 (0x8000 + 0x50)
    uint32_t sector_count =
        (uint32_t)p[0x8050]        |
        ((uint32_t)p[0x8051] << 8) |
        ((uint32_t)p[0x8052] << 16)|
        ((uint32_t)p[0x8053] << 24);

    if (sector_count == 0u) return false;

    kprintf("[LAUNCHER] QEMU loader ISO at 0x%08lx: %u sectors\n",
            (unsigned long)QEMU_ISO_PHYS, (unsigned)sector_count);

    // The data sits in physical RAM — pass it directly (no copy needed).
    return bellatrix_machine_insert_iso(p, (size_t)sector_count * ISO_SECTOR_SIZE) == 0;
}

// ---------------------------------------------------------------------------
// main launcher
// ---------------------------------------------------------------------------

bool launcher_run(void)
{
    if (!framebuffer || fb_width == 0u || fb_height == 0u) {
        kprintf("[LAUNCHER] framebuffer not ready\n");
        return false;
    }

    fb_init_stride();

    launcher_input_init();
    launcher_input_set_active(true);

    // Pump USB to give the keyboard and MSC device time to enumerate
    draw_message("Initialising...", COL_TITLE_BG);
    for (uint32_t i = 0u; i < 50u; i++) {
        pump_usb();
        for (volatile uint32_t d = 0u; d < 100000u; d++) asm volatile("nop");
    }

    // QEMU: images injected via -device loader land at fixed physical addresses.
    // On real hardware these locations contain random RAM and the magic checks fail.
    if (try_qemu_loader_adf()) {
        launcher_input_set_active(false);
        draw_message("QEMU: ADF loaded from memory.  Starting emulation...", COL_STATUS_BG);
        for (volatile uint32_t i = 0u; i < 3000000u; i++) asm volatile("nop");
        return true;
    }
    if (try_qemu_loader_iso()) {
        launcher_input_set_active(false);
        draw_message("QEMU: ISO loaded from memory.  Starting emulation...", COL_STATUS_BG);
        for (volatile uint32_t i = 0u; i < 3000000u; i++) asm volatile("nop");
        return true;
    }

    static char       s_adf_names[MAX_ADF_FILES][FAT32_NAME_MAX];
    static char       s_iso_names[MAX_ISO_FILES][FAT32_NAME_MAX];
    static MediaEntry s_entries[(MAX_ADF_FILES + MAX_ISO_FILES) * 2u];
    uint32_t count = 0u;

#if BELLATRIX_ENABLE_USBSTACK
    // Wait for USB MSC to enumerate (up to ~1 s additional)
    draw_message("Scanning USB drive...", COL_TITLE_BG);
    for (uint32_t i = 0u; i < 200u; i++) {
        pump_usb();
        for (volatile uint32_t d = 0u; d < 50000u; d++) asm volatile("nop");
        if (usb_msc_is_ready()) break;
    }

    if (usb_msc_is_ready() && fat32_init_usb(&s_fat32)) {
        uint32_t n_adf = fat32_list_adf(&s_fat32, s_adf_names, MAX_ADF_FILES);
        uint32_t n_iso = fat32_list_iso(&s_fat32, s_iso_names, MAX_ISO_FILES);
        kprintf("[LAUNCHER] USB: %u ADF, %u ISO\n", (unsigned)n_adf, (unsigned)n_iso);

        for (uint32_t i = 0u; i < n_adf; i++) {
            memcpy(s_entries[count].name, s_adf_names[i], FAT32_NAME_MAX);
            s_entries[count].type = MEDIA_ADF;
            count++;
        }
        for (uint32_t i = 0u; i < n_iso; i++) {
            memcpy(s_entries[count].name, s_iso_names[i], FAT32_NAME_MAX);
            s_entries[count].type = MEDIA_ISO;
            count++;
        }
    }
#endif

    if (count == 0u) {
        kprintf("[LAUNCHER] no media found on USB drive\n");
        draw_message("No media on USB drive.  Press any key (or wait) to boot without disk.", COL_STATUS_BG);
        wait_ack();
        launcher_input_set_active(false);
        return false;
    }

    kprintf("[LAUNCHER] found %u entries\n", (unsigned)count);

    // Flush any keys queued during initialisation
    while (launcher_input_pop() != 0u) {}

    uint32_t cursor = 0u;
    uint32_t scroll = 0u;
    bool     done   = false;
    bool     ok     = false;

    draw_frame(count, cursor, scroll, s_entries);

    while (!done) {
        pump_usb();

        uint8_t key = launcher_input_pop();
        if (key == 0u) continue;

        switch (key) {
        case LAUNCHER_KEY_UP:
            if (cursor > 0u) {
                cursor--;
                if (cursor < scroll) scroll = cursor;
                draw_frame(count, cursor, scroll, s_entries);
            }
            break;

        case LAUNCHER_KEY_DOWN:
            if (cursor + 1u < count) {
                cursor++;
                if (cursor >= scroll + VISIBLE_ROWS) scroll = cursor - VISIBLE_ROWS + 1u;
                draw_frame(count, cursor, scroll, s_entries);
            }
            break;

        case LAUNCHER_KEY_ENTER:
        case LAUNCHER_KEY_KPENTER:
            done = true;
            ok   = true;
            break;

        case LAUNCHER_KEY_ESC:
            done = true;
            ok   = false;
            break;

        default:
            break;
        }
    }

    launcher_input_set_active(false);

    if (!ok) {
        kprintf("[LAUNCHER] user chose to boot without disk\n");
        draw_message("Booting without disk...", COL_STATUS_BG);
        return false;
    }

    const MediaEntry *sel = &s_entries[cursor];

#if BELLATRIX_ENABLE_USBSTACK
    Fat32State *sel_fs       = &s_fat32;
    Fat32File  *sel_iso_file = &s_iso_file;
    const char *src_tag      = "USB";
#else
    /* count is always 0 without USB stack — unreachable */
    launcher_input_set_active(false);
    return false;
#endif

    if (sel->type == MEDIA_ADF) {
        // ADF: preload entire image into RAM buffer
        kprintf("[LAUNCHER] loading ADF \"%s\" from %s...\n", sel->name, src_tag);
        draw_message("Loading ADF...", COL_TITLE_BG);

        Fat32File file;
        if (!fat32_open(sel_fs, sel->name, &file)) {
            kprintf("[LAUNCHER] open failed\n");
            return false;
        }
        if (file.file_size > ADF_BUF_SIZE) {
            kprintf("[LAUNCHER] ADF too large (%u bytes)\n", (unsigned)file.file_size);
            return false;
        }
        if (!fat32_read_all(&file, s_adf_buf, ADF_BUF_SIZE)) {
            kprintf("[LAUNCHER] read failed\n");
            return false;
        }

        int rc = bellatrix_machine_insert_df0_adf(s_adf_buf, file.file_size);
        if (!rc) {
            kprintf("[LAUNCHER] insert_df0_adf failed\n");
            return false;
        }

        kprintf("[LAUNCHER] DF0: \"%s\" (%u bytes) [%s]\n", sel->name, (unsigned)file.file_size, src_tag);
        draw_message("Disk inserted.  Starting emulation...", COL_STATUS_BG);

    } else {
        // ISO: open file and attach via read callback — no preload
        kprintf("[LAUNCHER] attaching ISO \"%s\" via CD-ROM [%s]...\n", sel->name, src_tag);
        draw_message("Attaching ISO...", COL_TITLE_BG);

        if (!fat32_open(sel_fs, sel->name, sel_iso_file)) {
            kprintf("[LAUNCHER] open failed\n");
            return false;
        }

        uint32_t sector_count = sel_iso_file->file_size / ISO_SECTOR_SIZE;
        if (sector_count == 0u) {
            kprintf("[LAUNCHER] ISO too small (%u bytes)\n", (unsigned)sel_iso_file->file_size);
            return false;
        }

        int rc = bellatrix_machine_attach_iso_fn(fat32_iso_read_cb,
                                                 sel_iso_file,
                                                 sector_count);
        if (rc != 0) {
            kprintf("[LAUNCHER] attach_iso_fn failed (%d)\n", rc);
            return false;
        }

        kprintf("[LAUNCHER] CD-ROM: \"%s\" (%u sectors) [%s]\n", sel->name, (unsigned)sector_count, src_tag);
        draw_message("ISO attached.  Starting emulation...", COL_STATUS_BG);
    }

    for (volatile uint32_t i = 0; i < 5000000u; i++) asm volatile("nop");
    return true;
}
