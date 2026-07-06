// src/launcher/launcher.c
// Media selector UI for Bellatrix bare-metal.
//
// Shows a list of .ISO (CD-ROM), .HDF (hard disk) and up to four .ADF
// (floppy) files found on a USB pen drive.  Navigation: UP/DOWN arrows,
// SPACE to toggle, ENTER to boot the selected set, ESC to boot without disk.
//
// ADF: loaded entirely into static RAM buffers, inserted as DF0-DF3 via
//      bellatrix_machine_insert_df_adf().
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
#if BELLATRIX_ENABLE_BTSTACK
#include "io/bluetooth/bt_scan.h"
#include "io/bluetooth/bt_pairs.h"
#include "io/bluetooth/bt_diag.h"
#include "io/bluetooth/bt_link_key_db_sd.h"
#include "storage/sdcard/bcm_emmc.h"
#endif

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

int      kprintf(const char *fmt, ...);
int      bellatrix_machine_insert_df_adf(unsigned drive, const uint8_t *adf,
                                         uint32_t adf_size);
int      bellatrix_machine_insert_df0_adf(const uint8_t *adf, uint32_t adf_size);
int      bellatrix_machine_insert_iso(const void *data, size_t size);
int      bellatrix_machine_attach_iso_fn(iso_read_fn fn, void *ctx, uint32_t sector_count);
int      bellatrix_machine_attach_hdf_fn(int (*read_fn)(void *ctx, uint32_t lba, uint32_t count,
                                                        uint8_t *buf),
                                         void *ctx, uint32_t sector_count);
void     bellatrix_launcher_pump_usb(void);
void     bellatrix_launcher_pump_bt(void);
int      bellatrix_launcher_bt_ready(void);
uint32_t bt_hal_raspi3_io_activity(void);
uint32_t bt_hal_raspi3_io_tx(void);
uint32_t bt_hal_raspi3_io_rx(void);
void     bt_hal_raspi3_rx_pending(uint32_t *filled, uint32_t *wanted);
#if BELLATRIX_ENABLE_BTSTACK
void     bt_hal_raspi3_drain_fifo(void);
#endif

extern uint16_t *framebuffer;
extern uint32_t  pitch;
extern uint32_t  fb_width;
extern uint32_t  fb_height;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MAX_FILES       96u
#define MAX_ADF_SELECTIONS 4u
#define ADF_BUF_SIZE    (1024u * 1024u)   // 1 MB — large enough for any DD/HD ADF

// Combined media list entry
typedef enum {
    MEDIA_ISO = 0,
    MEDIA_HDF,
    MEDIA_ADF,
} MediaType;

typedef struct {
    char      name[FAT32_NAME_MAX];
    MediaType type;
    bool      selected;
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

// Font scale and derived layout metrics, set from the framebuffer size so
// text stays readable on high resolutions (8 px glyphs are tiny at 1080p).
static uint32_t s_scale;        // integer glyph magnification (1..3)
static uint32_t s_char;         // glyph cell size in pixels (8 * s_scale)
static uint32_t s_margin_x;     // left margin of the file list
static uint32_t s_title_h;      // title bar height
static uint32_t s_row_h;        // file list row height
static uint32_t s_status_h;     // status bar height
static uint32_t s_visible_rows; // entries visible on screen at once

static void ui_init_metrics(void)
{
    s_stride = pitch / 2u;

    s_scale = fb_width / 640u;
    if (s_scale < 1u) s_scale = 1u;
    if (s_scale > 3u) s_scale = 3u;

    s_char     = 8u * s_scale;
    s_margin_x = 2u * s_char;
    s_title_h  = s_char + 12u;
    s_row_h    = s_char + 4u;
    s_status_h = s_char + 6u;

    uint32_t list_h = fb_height - s_title_h - 4u - s_status_h;
    s_visible_rows  = list_h / s_row_h;
    if (s_visible_rows == 0u) s_visible_rows = 1u;
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
#if BELLATRIX_ENABLE_BTSTACK
        /* Drain the PL011 FIFO into the BT ring buffer after each scanline so
         * that slow framebuffer writes (uncached MMIO) do not create gaps long
         * enough to overflow the 16-byte hardware FIFO and desync the H4 parser. */
        bt_hal_raspi3_drain_fifo();
#endif
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
        uint8_t bits = glyph[row];
        for (uint32_t sy = 0u; sy < s_scale; sy++) {
            uint16_t *dst = framebuffer + (py + row * s_scale + sy) * s_stride + px;
            for (unsigned col = 0u; col < 8u; col++) {
                uint16_t c = (bits & (0x80u >> col)) ? fg : bg;
                for (uint32_t sx = 0u; sx < s_scale; sx++) {
                    dst[col * s_scale + sx] = c;
                }
            }
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
        x += s_char;
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
    uint32_t w   = len * s_char;
    uint32_t mid = x0 + (x1 - x0) / 2u;
    uint32_t px  = (w < (x1 - x0)) ? (mid - w / 2u) : x0;

    // Fill background for the full row width
    fb_fill_rect(x0, y, x1 - x0, s_char, bg);
    fb_puts(px, y, s, fg, bg);
}

// ---------------------------------------------------------------------------
// Media type filter — the FAT32 layer lists everything; deciding what is
// bootable media is the launcher's call.
// ---------------------------------------------------------------------------

#if BELLATRIX_ENABLE_USBSTACK
static bool name_has_ext(const char *name, const char *ext /* e.g. ".adf" */)
{
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '.') dot = p;
    }
    if (!dot) return false;

    const char *a = dot, *b = ext;
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == *b;
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int ascii_strcasecmp(const char *a, const char *b)
{
    while (*a || *b) {
        char ca = ascii_lower(*a);
        char cb = ascii_lower(*b);
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        if (*a) a++;
        if (*b) b++;
    }
    return 0;
}

static void sort_media_entries(MediaEntry *entries, uint32_t count)
{
    for (uint32_t i = 1u; i < count; i++) {
        MediaEntry key = entries[i];
        uint32_t j = i;
        while (j > 0u) {
            const MediaEntry *prev = &entries[j - 1u];
            bool after_key =
                (prev->type > key.type) ||
                (prev->type == key.type &&
                 ascii_strcasecmp(prev->name, key.name) > 0);
            if (!after_key) break;
            entries[j] = entries[j - 1u];
            j--;
        }
        entries[j] = key;
    }
}

#endif /* BELLATRIX_ENABLE_USBSTACK */

static void toggle_media_selection(MediaEntry *entries, uint32_t count, uint32_t cursor)
{
    if (cursor >= count) return;
    MediaEntry *entry = &entries[cursor];
    bool select = !entry->selected;

    if (select) {
        if (entry->type == MEDIA_ADF) {
            uint32_t selected_adfs = 0u;
            for (uint32_t i = 0u; i < count; i++) {
                if (entries[i].type == MEDIA_ADF && entries[i].selected)
                    selected_adfs++;
            }
            if (selected_adfs >= MAX_ADF_SELECTIONS)
                return;
        } else {
            for (uint32_t i = 0u; i < count; i++) {
                if (entries[i].type == entry->type)
                    entries[i].selected = false;
            }
        }
    }

    entry->selected = select;
}

static bool any_media_selected(const MediaEntry *entries, uint32_t count)
{
    for (uint32_t i = 0u; i < count; i++) {
        if (entries[i].selected) return true;
    }
    return false;
}

static const char *media_tag(MediaType type)
{
    switch (type) {
    case MEDIA_ISO: return "[ISO] ";
    case MEDIA_HDF: return "[HDF] ";
    default:        return "[ADF] ";
    }
}

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
    fb_fill_rect(0, 0, W, s_title_h, COL_TITLE_BG);
    fb_puts_centred(0, W, (s_title_h - s_char) / 2u,
                    "BELLATRIX LAUNCHER  --  Select media (ISO, HDF, up to 4 ADF):",
                    COL_TEXT, COL_TITLE_BG);

    // File list
    uint32_t list_y = s_title_h + 4u;

    for (uint32_t i = 0u; i < s_visible_rows && (scroll + i) < count; i++) {
        uint32_t         idx      = scroll + i;
        const MediaEntry *e       = &entries[idx];
        bool              selected = (idx == cursor);

        uint16_t bg  = selected ? COL_CURSOR_BG : COL_BG;
        uint16_t fg  = selected ? COL_TEXT_SEL  : COL_TEXT;

        uint32_t row_y = list_y + i * s_row_h;

        fb_fill_rect(0, row_y, W, s_row_h, bg);

        if (selected) {
            fb_putchar(s_margin_x - s_char - 2u, row_y + 2u, '>', fg, bg);
        }

        char mark[5] = { '[', e->selected ? '*' : ' ', ']', ' ', '\0' };
        uint32_t x = fb_puts(s_margin_x, row_y + 2u, mark, fg, bg);

        // Type tag
        const char *tag = media_tag(e->type);
        x = fb_puts(x, row_y + 2u, tag, COL_HINT, bg);
        fb_puts(x, row_y + 2u, e->name, fg, bg);
    }

    // Status bar
    uint32_t status_y = H - s_status_h;
    fb_fill_rect(0, status_y, W, s_status_h, COL_STATUS_BG);

    char hint[160];
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
    const char *suffix = "  |  UP/DOWN: navigate  |  SPACE: mark  |  ENTER: boot  |  ESC: no disk";
    for (const char *p = suffix; *p; p++) hint[n_chars++] = *p;
    hint[n_chars] = '\0';

    fb_puts_centred(0, W, status_y + (s_status_h - s_char) / 2u,
                    hint, COL_TEXT, COL_STATUS_BG);
}

static void draw_message(const char *msg, uint16_t bg_col)
{
    if (!framebuffer) return;

    const uint32_t W = fb_width;
    const uint32_t H = fb_height;

    fb_fill_rect(0, 0, W, H, COL_BG);
    fb_fill_rect(0, H / 2u - (s_char + 8u), W, 2u * (s_char + 8u), bg_col);
    fb_puts_centred(0, W, H / 2u - s_char / 2u, msg, COL_TEXT, bg_col);
}

// ---------------------------------------------------------------------------
// ADF buffer
// ---------------------------------------------------------------------------

static uint8_t s_adf_buf[MAX_ADF_SELECTIONS][ADF_BUF_SIZE] __attribute__((aligned(512)));

// ---------------------------------------------------------------------------
// FAT32 state + ISO file — kept alive for the duration of emulation so that
// the ATAPI read callback can seek and read sectors on demand.
// ---------------------------------------------------------------------------

#if BELLATRIX_ENABLE_USBSTACK
static Fat32State s_fat32;
static Fat32File  s_iso_file;
static Fat32File  s_hdf_file;
#endif

#if BELLATRIX_ENABLE_USBSTACK
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

static int fat32_hdf_read_cb(void *ctx, uint32_t lba, uint32_t count, uint8_t *dst)
{
    Fat32File *f = (Fat32File *)ctx;
    uint32_t off = lba * 512u;
    uint32_t want = count * 512u;

    if (count == 0u) return 0;
    if (lba > f->file_size / 512u) return -1;
    if (count > (f->file_size / 512u) - lba) return -1;
    if (!fat32_seek(f, off)) return -1;

    return fat32_read(f, dst, want) == want ? 0 : -1;
}
#endif /* BELLATRIX_ENABLE_USBSTACK */

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
// Bluetooth scan screen (diagnostic / pairing front-end)
//
// The serial console is dark while BTStack owns the PL011, so discovery
// results can only be shown here, on the framebuffer.
// ---------------------------------------------------------------------------

#if BELLATRIX_ENABLE_BTSTACK

static char bt_hex_digit(uint8_t v)
{
    return (char)(v < 10u ? (int)'0' + (int)v : (int)'A' + (int)v - 10);
}

// "11:22:33:44:55:66" — out must hold 18 bytes
static void bt_format_addr(char *out, const uint8_t *a)
{
    for (unsigned i = 0u; i < 6u; i++) {
        out[i * 3u]      = bt_hex_digit((uint8_t)(a[i] >> 4));
        out[i * 3u + 1u] = bt_hex_digit((uint8_t)(a[i] & 0x0Fu));
        out[i * 3u + 2u] = (i < 5u) ? ':' : '\0';
    }
}

/* Fill in horizontal bands, draining the BT UART between bands: a full
 * 1920x1080 fill takes tens of ms and the 16-byte PL011 RX FIFO overruns
 * in ~1.4ms at 115200 — lost bytes desync the H4 parser (phantom devices). */
static void bt_fill_rect_pumped(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                uint32_t col)
{
    const uint32_t band = 32u;
    while (h) {
        uint32_t hh = (h < band) ? h : band;
        fb_fill_rect(x, y, w, hh, col);
        bellatrix_launcher_pump_bt();
        y += hh;
        h -= hh;
    }
}

// Static parts of the scan screen — drawn ONCE.  Full-screen fills on
// the uncached framebuffer cost several ms even in pumped bands, and a
// redraw per discovered device was the last blind window that lost HCI
// RX bytes (the chip outruns the 16-byte PL011 FIFO in ~1.4 ms).
static void bt_draw_scan_chrome(void)
{
    const uint32_t W = fb_width;
    const uint32_t H = fb_height;

    /* build tag: date + feature flags so we can confirm which binary is running */
    static const char build_tag[] =
        "build:" __DATE__ " " __TIME__
#if BELLATRIX_ENABLE_BTSTACK
        " BT"
#endif
#if BELLATRIX_ENABLE_USBSTACK
        " USB"
#endif
        ;

    bt_fill_rect_pumped(0, 0, W, H, COL_BG);
    bt_fill_rect_pumped(0, 0, W, s_title_h, COL_TITLE_BG);
    fb_puts_centred(0, W, (s_title_h - s_char) / 2u,
                    "BLUETOOTH SCAN  --  put your devices in pairing mode",
                    COL_TEXT, COL_TITLE_BG);

    bt_fill_rect_pumped(0, H - s_status_h, W, s_status_h, COL_STATUS_BG);
    /* status bar: left = build tag, right = key hint */
    fb_puts(s_margin_x, H - s_status_h + (s_status_h - s_char) / 2u,
            build_tag, COL_TEXT, COL_STATUS_BG);
    fb_puts_centred(0, W, H - s_status_h + (s_status_h - s_char) / 2u,
                    "UP/DOWN=select  ENTER=pair/unpair  ESC=boot",
                    COL_TEXT, COL_STATUS_BG);
}

static unsigned s_bt_cursor; /* selected row index in scan list */

/* True if addr from bt_pairs has already been found by the current inquiry. */
static bool scan_has_addr(const uint8_t addr[6])
{
    unsigned n = bt_scan_count();
    for (unsigned i = 0u; i < n; i++) {
        const BTScanResult *r = bt_scan_get(i);
        if (r && memcmp(r->addr, addr, 6u) == 0) return true;
    }
    return false;
}

// Dynamic rows, overwritten in place: every line is padded to a fixed
// width so stale text disappears without clearing the background.
static void bt_draw_scan_rows(void)
{
    const uint32_t H = fb_height;
    uint32_t y = s_title_h + 4u;
    char line[64];
    unsigned p;

    p = 0u;
    for (const char *s = bt_scan_status(); *s && p < sizeof(line) - 1u; s++)
        line[p++] = *s;
    while (p < sizeof(line) - 1u) line[p++] = ' ';
    line[p] = '\0';
    fb_puts(s_margin_x, y, line, COL_CURSOR_BG, COL_BG);
    bellatrix_launcher_pump_bt();
    y += s_row_h + s_row_h / 2u;

    /* Saved pairs not yet found by inquiry — show immediately on boot */
    unsigned pair_count = bt_pairs_count();
    for (unsigned i = 0u; i < pair_count; i++) {
        const BTPair *pair = bt_pairs_get(i);
        if (!pair || scan_has_addr(pair->addr)) continue;
        if (y + s_row_h + s_status_h >= H) break;
        p = 0u;
        static const char *type_char[4] = { "?", "K", "M", "J" };
        /* "  S CL 12:34:21:ED:D1:1E name" — S = Saved, not yet visible */
        line[p++] = ' '; line[p++] = 'S'; line[p++] = ' ';
        line[p++] = type_char[pair->device_type < 4u ? pair->device_type : 0u][0];
        line[p++] = ' ';
        bt_format_addr(&line[p], pair->addr);
        p += 17u;
        line[p++] = ' ';
        const char *name = pair->name[0] ? pair->name : "(no name)";
        for (const char *s = name; *s && p < sizeof(line) - 1u; s++)
            line[p++] = *s;
        while (p < sizeof(line) - 1u) line[p++] = ' ';
        line[p] = '\0';
        fb_puts(s_margin_x, y, line, COL_STATUS_BG, COL_BG);
        bellatrix_launcher_pump_bt();
        y += s_row_h;
    }

    unsigned count = bt_scan_count();

    for (unsigned i = 0u; i < count || i == 0u; i++) {
        const BTScanResult *r = (count > 0u) ? bt_scan_get(i) : NULL;
        if (count > 0u && !r) break;
        if (y + s_row_h + s_status_h >= H) break;

        p = 0u;
        if (!r) {
            for (const char *s = "(no devices found yet)"; *s; s++)
                line[p++] = *s;
        } else {
            static const char *transport_tag[4] = { "??", "CL", "LE", "DM" };
            const char *tag = transport_tag[r->transport & 3u];
            bool paired = bt_pairs_is_known(r->addr);

            /* cursor arrow */
            line[p++] = (i == s_bt_cursor) ? '>' : ' ';
            /* paired marker */
            line[p++] = paired ? 'P' : ' ';
            /* HID marker */
            line[p++] = r->hid ? '*' : ' ';
            line[p++] = tag[0];
            line[p++] = tag[1];
            line[p++] = ' ';
            bt_format_addr(&line[p], r->addr);
            p += 17u;
            line[p++] = ' ';

            const char *name = r->name[0] ? r->name : "(no name)";
            for (const char *s = name; *s && p < sizeof(line) - 1u; s++)
                line[p++] = *s;
        }
        while (p < sizeof(line) - 1u) line[p++] = ' ';
        line[p] = '\0';

        /* highlight cursor row */
        uint16_t fg = (i == s_bt_cursor) ? COL_BG        : COL_TEXT;
        uint16_t bg = (i == s_bt_cursor) ? COL_CURSOR_BG : COL_BG;
        fb_puts(s_margin_x, y, line, fg, bg);
        bellatrix_launcher_pump_bt();
        y += s_row_h;
    }
}

// Append a string to a bounded text buffer; returns new length.
static uint32_t txt_append(char *buf, uint32_t len, uint32_t cap, const char *s)
{
    while (*s && len + 1u < cap) buf[len++] = *s++;
    buf[len] = '\0';
    return len;
}

// Format the scan results + bring-up diagnostics and overwrite BTSCAN.TXT
// on the SD card (in place — the file must already exist, any size).
#define BT_REPORT_CAP 12288u
static char s_bt_report[BT_REPORT_CAP];

static bool sd_read_block_cb(void *ctx, uint32_t lba, uint8_t *buf)
{
    (void)ctx;
    return bcm_emmc_read_block(lba, buf);
}

static bool sd_write_block_cb(void *ctx, uint32_t lba, const uint8_t *buf)
{
    (void)ctx;
    return bcm_emmc_write_block(lba, buf);
}

// Returns: 1 saved, 0 file missing, -1 SD/FS error.
static int bt_save_report_to_sd(void);

void launcher_save_bt_report(void) { bt_save_report_to_sd(); }

static int bt_save_report_to_sd(void)
{
    uint32_t len = 0u;

    len = txt_append(s_bt_report, len, BT_REPORT_CAP,
                     "=== BELLATRIX BLUETOOTH REPORT ===\r\n\r\n--- scan results ---\r\n");

    unsigned count = bt_scan_count();
    if (count == 0u)
        len = txt_append(s_bt_report, len, BT_REPORT_CAP, "(no devices found)\r\n");

    for (unsigned i = 0u; i < count; i++) {
        const BTScanResult *r = bt_scan_get(i);
        char addr[18];
        if (!r) break;
        static const char *transport_tag[4] = { "?? ", "CL ", "LE ", "DM " };
        bt_format_addr(addr, r->addr);
        len = txt_append(s_bt_report, len, BT_REPORT_CAP,
                         transport_tag[r->transport & 3u]);
        len = txt_append(s_bt_report, len, BT_REPORT_CAP, addr);
        len = txt_append(s_bt_report, len, BT_REPORT_CAP, r->hid ? " [HID] " : " ");
        len = txt_append(s_bt_report, len, BT_REPORT_CAP,
                         r->name[0] ? r->name : "(no name)");
        len = txt_append(s_bt_report, len, BT_REPORT_CAP, "\r\n");
    }

    /* Saved pairs — shows what BTPAIRS.TXT contained at boot */
    len = txt_append(s_bt_report, len, BT_REPORT_CAP, "\r\n--- saved pairs (BTPAIRS.TXT) ---\r\n");
    unsigned pair_count = bt_pairs_count();
    if (pair_count == 0u) {
        len = txt_append(s_bt_report, len, BT_REPORT_CAP, "(none)\r\n");
    } else {
        static const char *type_tag[4] = { "?", "K", "M", "J" };
        static const char *tr_tag[4]   = { "??", "CL", "LE", "DM" };
        for (unsigned i = 0u; i < pair_count; i++) {
            const BTPair *p = bt_pairs_get(i);
            if (!p) break;
            char paddr[18];
            bt_format_addr(paddr, p->addr);
            len = txt_append(s_bt_report, len, BT_REPORT_CAP,
                             tr_tag[p->transport & 3u]);
            len = txt_append(s_bt_report, len, BT_REPORT_CAP, " ");
            len = txt_append(s_bt_report, len, BT_REPORT_CAP,
                             type_tag[p->device_type < 4u ? p->device_type : 0u]);
            len = txt_append(s_bt_report, len, BT_REPORT_CAP, " ");
            len = txt_append(s_bt_report, len, BT_REPORT_CAP, paddr);
            len = txt_append(s_bt_report, len, BT_REPORT_CAP, " ");
            len = txt_append(s_bt_report, len, BT_REPORT_CAP,
                             p->name[0] ? p->name : "(no name)");
            len = txt_append(s_bt_report, len, BT_REPORT_CAP, "\r\n");
        }
    }

    len = txt_append(s_bt_report, len, BT_REPORT_CAP, "\r\n--- bring-up log ---\r\n");
    len += bt_diag_snapshot(s_bt_report + len,
                            (BT_REPORT_CAP > len) ? BT_REPORT_CAP - len : 0u);

    static Fat32State s_sd_fs;
    if (!bcm_emmc_init() ||
        !fat32_init_with_reader(&s_sd_fs, sd_read_block_cb, NULL))
        return -1;
    fat32_set_writer(&s_sd_fs, sd_write_block_cb, NULL);

    if (!fat32_overwrite_in_place(&s_sd_fs, "BTSCAN.TXT", s_bt_report, len)) {
        Fat32File probe;
        return fat32_open(&s_sd_fs, "BTSCAN.TXT", &probe) ? -1 : 0;
    }

    /* Save link keys if a new pairing happened since last write. */
    if (bt_link_key_db_sd_dirty()) {
        static char keys_buf[BT_LINK_KEY_FILE_CAP];
        bt_link_key_db_sd_serialise(keys_buf, BT_LINK_KEY_FILE_CAP);
        fat32_overwrite_in_place(&s_sd_fs, BT_LINK_KEY_FILENAME,
                                 keys_buf, BT_LINK_KEY_FILE_CAP - 1u);
        bt_link_key_db_sd_clear_dirty();
    }

    return 1;
}

/* Load BTPAIRS.TXT and BTKEYS.TXT from SD.  Reuses s_bt_report as read buffer. */
static void bt_pairs_load_from_sd(Fat32State *fs)
{
    Fat32File f;
    if (!fat32_open(fs, BT_PAIRS_FILENAME, &f)) {
        bt_pairs_load(NULL, 0u);
    } else {
        uint32_t got = fat32_read(&f, s_bt_report, BT_REPORT_CAP - 1u);
        s_bt_report[got] = '\0';
        bt_pairs_load(s_bt_report, got);
    }

    static char s_keys_buf[BT_LINK_KEY_FILE_CAP];
    Fat32File kf;
    if (!fat32_open(fs, BT_LINK_KEY_FILENAME, &kf)) {
        bt_link_key_db_sd_load(NULL, 0u);
    } else {
        uint32_t got = fat32_read(&kf, s_keys_buf, BT_LINK_KEY_FILE_CAP - 1u);
        s_keys_buf[got] = '\0';
        bt_link_key_db_sd_load(s_keys_buf, got);
    }
}

/* Write BTPAIRS.TXT and BTKEYS.TXT back to SD if changed. */
static void bt_pairs_save_to_sd(Fat32State *fs)
{
    if (bt_pairs_dirty()) {
        static char pairs_buf[BT_PAIRS_FILE_CAP];
        bt_pairs_serialise(pairs_buf, BT_PAIRS_FILE_CAP);
        fat32_overwrite_in_place(fs, BT_PAIRS_FILENAME, pairs_buf, BT_PAIRS_FILE_CAP - 1u);
    }
    if (bt_link_key_db_sd_dirty()) {
        static char keys_buf[BT_LINK_KEY_FILE_CAP];
        bt_link_key_db_sd_serialise(keys_buf, BT_LINK_KEY_FILE_CAP);
        fat32_overwrite_in_place(fs, BT_LINK_KEY_FILENAME, keys_buf, BT_LINK_KEY_FILE_CAP - 1u);
        bt_link_key_db_sd_clear_dirty();
    }
}

// Run the scan screen until the user continues or ~90 s elapse.
// Shown even when the controller bootstrap failed, so the failure is
// visible (the serial console is dark during BT operation) and the
// diagnostics still get saved to the SD card.
static void bt_scan_screen(void)
{
    bool working = bellatrix_launcher_bt_ready() != 0;

    /* Mount SD early so we can load BTPAIRS.TXT before the scan starts. */
    static Fat32State s_sd_fs_scan;
    bool sd_ok = bcm_emmc_init() &&
                 fat32_init_with_reader(&s_sd_fs_scan, sd_read_block_cb, NULL);
    if (sd_ok) {
        fat32_set_writer(&s_sd_fs_scan, sd_write_block_cb, NULL);
        bt_pairs_load_from_sd(&s_sd_fs_scan);
    }

    s_bt_cursor = 0u;

    if (working)
        bt_scan_start();

    bt_draw_scan_chrome();
    if (!working)
        fb_puts(s_margin_x, s_title_h + 4u,
                "controller bootstrap FAILED - report goes to BTSCAN.TXT",
                COL_CURSOR_BG, COL_BG);

    uint32_t last_gen = working ? 0xFFFFFFFFu : bt_scan_generation();
    // Outer iteration ≈ 0.125 ms — tight cadence keeps the 16-byte PL011
    // FIFO drained during LE advert floods (~10KB/s at 115200).
    uint32_t budget   = working ? 720000u : 96000u;   // ≈90 s / ≈12 s
    bool needs_redraw = false;

    for (uint32_t iter = 0u; iter < budget; iter++) {
        bellatrix_launcher_pump_bt();
        // USB pump can hold the CPU for >1.4 ms (a full PL011 RX FIFO at
        // 115200) — keep it rare; ~64 ms of input latency is invisible.
        if ((iter & 511u) == 0u) pump_usb();

        // ~2s heartbeat into the SD report: UART byte counter shows whether
        // the link is moving at all while the scan appears silent.
        if (working && iter != 0u && (iter & 16383u) == 0u) {
            uint32_t rxq_filled, rxq_wanted;
            bt_hal_raspi3_rx_pending(&rxq_filled, &rxq_wanted);
            bt_diag_log("[SCAN] hb iter=%u tx=%u rx=%u rxq=%u/%u status=%s found=%u\n",
                        (unsigned)iter,
                        (unsigned)bt_hal_raspi3_io_tx(),
                        (unsigned)bt_hal_raspi3_io_rx(),
                        (unsigned)rxq_filled, (unsigned)rxq_wanted,
                        bt_scan_status(), bt_scan_count());
        }

        uint8_t key = launcher_input_pop();
        if (key == LAUNCHER_KEY_ESC)
            break;

        if (key == LAUNCHER_KEY_ENTER || key == LAUNCHER_KEY_KPENTER) {
            unsigned count = bt_scan_count();
            if (count > 0u) {
                const BTScanResult *r = bt_scan_get(s_bt_cursor);
                if (r) {
                    if (bt_pairs_is_known(r->addr))
                        bt_pairs_remove(r->addr);
                    else
                        bt_pairs_add(r);
                    needs_redraw = true;
                }
            } else {
                break; /* ENTER with no devices = exit */
            }
        }

        if (key == LAUNCHER_KEY_DEL || key == LAUNCHER_KEY_BKSP) {
            unsigned count = bt_scan_count();
            if (count > 0u) {
                const BTScanResult *r = bt_scan_get(s_bt_cursor);
                if (r && bt_pairs_is_known(r->addr)) {
                    bt_pairs_remove(r->addr);
                    needs_redraw = true;
                }
            }
        }

        if (key == LAUNCHER_KEY_UP && s_bt_cursor > 0u) {
            s_bt_cursor--;
            needs_redraw = true;
        }
        if (key == LAUNCHER_KEY_DOWN) {
            unsigned count = bt_scan_count();
            if (s_bt_cursor + 1u < count) {
                s_bt_cursor++;
                needs_redraw = true;
            }
        }

        if ((iter & 4095u) == 0u && bt_scan_generation() != last_gen) {
            last_gen = bt_scan_generation();
            /* clamp cursor to current result count */
            unsigned count = bt_scan_count();
            if (count > 0u && s_bt_cursor >= count)
                s_bt_cursor = count - 1u;
            needs_redraw = true;
        }

        if (needs_redraw) {
            bt_draw_scan_rows();
            needs_redraw = false;
        }

        for (volatile uint32_t d = 0u; d < 100000u; d++) asm volatile("nop");
    }

    if (working) {
        bt_scan_stop();
        // Let the stack flush the inquiry/scan-stop commands before moving on.
        for (uint32_t i = 0u; i < 200u; i++) {
            bellatrix_launcher_pump_bt();
            for (volatile uint32_t d = 0u; d < 50000u; d++) asm volatile("nop");
        }
    }

    if (sd_ok)
        bt_pairs_save_to_sd(&s_sd_fs_scan);

    int saved = bt_save_report_to_sd();
    const char *msg =
        (saved == 1) ? "Report saved to SD:BTSCAN.TXT" :
        (saved == 0) ? "Create BTSCAN.TXT (any size, e.g. 16KB) on the SD card to save reports"
                     : "SD write failed - report not saved";
    draw_message(msg, (saved == 1) ? COL_STATUS_BG : COL_TITLE_BG);
    for (volatile uint32_t d = 0u; d < 2000000000u; d++) asm volatile("nop");
}

#endif // BELLATRIX_ENABLE_BTSTACK

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
    memcpy(s_adf_buf[0], p, ADF_SIZE_DD);
    return bellatrix_machine_insert_df0_adf(s_adf_buf[0], ADF_SIZE_DD) != 0;
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

    ui_init_metrics();

    launcher_input_init();
    launcher_input_set_active(true);

    // Pump USB to give the keyboard and MSC device time to enumerate
    draw_message("Initialising...", COL_TITLE_BG);
    for (uint32_t i = 0u; i < 50u; i++) {
        pump_usb();
        for (volatile uint32_t d = 0u; d < 100000u; d++) asm volatile("nop");
    }

#if BELLATRIX_ENABLE_BTSTACK
    bt_scan_screen();
#endif

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

    static MediaEntry s_entries[MAX_FILES];
    uint32_t count = 0u;

#if BELLATRIX_ENABLE_USBSTACK
    static char s_names[MAX_FILES][FAT32_NAME_MAX];

    // Wait for USB MSC to enumerate (up to ~1 s additional)
    draw_message("Scanning USB drive...", COL_TITLE_BG);
    for (uint32_t i = 0u; i < 200u; i++) {
        pump_usb();
        for (volatile uint32_t d = 0u; d < 50000u; d++) asm volatile("nop");
        if (usb_msc_is_ready()) break;
    }

    if (usb_msc_is_ready() && fat32_init_usb(&s_fat32)) {
        uint32_t n_files = fat32_list(&s_fat32, s_names, MAX_FILES);

        for (uint32_t i = 0u; i < n_files && count < MAX_FILES; i++) {
            MediaType type;
            if      (name_has_ext(s_names[i], ".iso")) type = MEDIA_ISO;
            else if (name_has_ext(s_names[i], ".hdf")) type = MEDIA_HDF;
            else if (name_has_ext(s_names[i], ".adf")) type = MEDIA_ADF;
            else continue;

            memcpy(s_entries[count].name, s_names[i], FAT32_NAME_MAX);
            s_entries[count].type = type;
            s_entries[count].selected = false;
            count++;
        }

        sort_media_entries(s_entries, count);

        kprintf("[LAUNCHER] USB: %u files, %u bootable media\n",
                (unsigned)n_files, (unsigned)count);
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
                if (cursor >= scroll + s_visible_rows) scroll = cursor - s_visible_rows + 1u;
                draw_frame(count, cursor, scroll, s_entries);
            }
            break;

        case LAUNCHER_KEY_ENTER:
        case LAUNCHER_KEY_KPENTER:
            if (!any_media_selected(s_entries, count))
                toggle_media_selection(s_entries, count, cursor);
            done = true;
            ok   = true;
            break;

        case LAUNCHER_KEY_SPACE:
            toggle_media_selection(s_entries, count, cursor);
            draw_frame(count, cursor, scroll, s_entries);
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

#if !BELLATRIX_ENABLE_USBSTACK
    /* count is always 0 without USB stack — cannot reach here */
    (void)s_entries;
    launcher_input_set_active(false);
    return false;
#else

    const MediaEntry *iso = NULL;
    const MediaEntry *hdf = NULL;
    const MediaEntry *adfs[MAX_ADF_SELECTIONS] = { 0 };
    uint32_t adf_count = 0u;

    for (uint32_t i = 0u; i < count; i++) {
        if (!s_entries[i].selected) continue;
        switch (s_entries[i].type) {
        case MEDIA_ADF:
            if (adf_count < MAX_ADF_SELECTIONS)
                adfs[adf_count++] = &s_entries[i];
            break;
        case MEDIA_ISO: iso = &s_entries[i]; break;
        case MEDIA_HDF: hdf = &s_entries[i]; break;
        }
    }

    if (hdf) {
        kprintf("[LAUNCHER] attaching HDF \"%s\" via IDE [USB]...\n", hdf->name);
        draw_message("Attaching HDF...", COL_TITLE_BG);

        if (!fat32_open(&s_fat32, hdf->name, &s_hdf_file)) {
            kprintf("[LAUNCHER] HDF open failed\n");
            return false;
        }
        if (s_hdf_file.file_size < 512u || (s_hdf_file.file_size & 511u) != 0u) {
            kprintf("[LAUNCHER] HDF size invalid (%u bytes)\n", (unsigned)s_hdf_file.file_size);
            return false;
        }

        uint32_t sector_count = s_hdf_file.file_size / 512u;
        int rc = bellatrix_machine_attach_hdf_fn(fat32_hdf_read_cb,
                                                 &s_hdf_file,
                                                 sector_count);
        if (rc != 0) {
            kprintf("[LAUNCHER] attach_hdf_fn failed (%d)\n", rc);
            return false;
        }

        kprintf("[LAUNCHER] HD: \"%s\" (%u sectors) [USB, read-only]\n",
                hdf->name, (unsigned)sector_count);
    }

    if (iso) {
        kprintf("[LAUNCHER] attaching ISO \"%s\" via CD-ROM [USB]...\n", iso->name);
        draw_message("Attaching ISO...", COL_TITLE_BG);

        if (!fat32_open(&s_fat32, iso->name, &s_iso_file)) {
            kprintf("[LAUNCHER] ISO open failed\n");
            return false;
        }

        uint32_t sector_count = s_iso_file.file_size / ISO_SECTOR_SIZE;
        if (sector_count == 0u) {
            kprintf("[LAUNCHER] ISO too small (%u bytes)\n", (unsigned)s_iso_file.file_size);
            return false;
        }

        int rc = bellatrix_machine_attach_iso_fn(fat32_iso_read_cb,
                                                 &s_iso_file,
                                                 sector_count);
        if (rc != 0) {
            kprintf("[LAUNCHER] attach_iso_fn failed (%d)\n", rc);
            return false;
        }

        kprintf("[LAUNCHER] CD-ROM: \"%s\" (%u sectors) [USB]\n",
                iso->name, (unsigned)sector_count);
    }

    for (uint32_t drive = 0u; drive < adf_count; drive++) {
        const MediaEntry *adf = adfs[drive];
        kprintf("[LAUNCHER] loading ADF \"%s\" into DF%u from USB...\n",
                adf->name, (unsigned)drive);
        draw_message("Loading ADF...", COL_TITLE_BG);

        Fat32File file;
        if (!fat32_open(&s_fat32, adf->name, &file)) {
            kprintf("[LAUNCHER] ADF open failed\n");
            return false;
        }
        if (file.file_size > ADF_BUF_SIZE) {
            kprintf("[LAUNCHER] ADF too large (%u bytes)\n", (unsigned)file.file_size);
            return false;
        }
        if (!fat32_read_all(&file, s_adf_buf[drive], ADF_BUF_SIZE)) {
            kprintf("[LAUNCHER] ADF read failed\n");
            return false;
        }

        int rc = bellatrix_machine_insert_df_adf(drive, s_adf_buf[drive], file.file_size);
        if (!rc) {
            kprintf("[LAUNCHER] insert_df%u_adf failed\n", (unsigned)drive);
            return false;
        }

        kprintf("[LAUNCHER] DF%u: \"%s\" (%u bytes) [USB]\n",
                (unsigned)drive, adf->name, (unsigned)file.file_size);
    }

    draw_message("Media attached.  Starting emulation...", COL_STATUS_BG);

    for (volatile uint32_t i = 0; i < 5000000u; i++) asm volatile("nop");
    return true;
#endif /* !BELLATRIX_ENABLE_USBSTACK */
}
