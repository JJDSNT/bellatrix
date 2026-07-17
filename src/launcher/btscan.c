// src/launcher/btscan.c
// Bluetooth scan / pairing screen for the launcher. See btscan.h.

#include "launcher/btscan.h"

#if BELLATRIX_ENABLE_BTSTACK

#include "launcher/launcher_ui.h"
#include "launcher/launcher_input.h"
#include "storage/fat/fat32.h"
#include "storage/sdcard/bcm_emmc.h"
#include "io/bluetooth/bt_scan.h"
#include "io/bluetooth/bt_pairs.h"
#include "io/bluetooth/bt_diag.h"
#include "io/bluetooth/bt_hal_raspi3.h"
#include "io/bluetooth/bt_link_key_db_sd.h"
#include "host/pal.h"

#include <string.h>
#include <stdint.h>

// BT service glue, implemented in the runtime IO layer.
void bellatrix_launcher_pump_bt(void);
void bellatrix_launcher_bt_open_pairing(void);
void bellatrix_launcher_bt_close_pairing(void);
int  bellatrix_launcher_bt_ready(void);
void bellatrix_launcher_bt_connect_now(void);
void bellatrix_launcher_bt_suspend_reconnect(int suspended);
void bellatrix_launcher_bt_prepare_pairing(const uint8_t addr[6]);
int  bellatrix_launcher_bt_mouse_connected(void);
int  bellatrix_launcher_bt_recovery_discovery_active(void);
void bellatrix_launcher_bt_claim_recovery_discovery(void);

static bool s_bt_runtime_active;
static bool s_bt_runtime_working;
static bool s_bt_runtime_sd_ok;
static bool s_bt_runtime_connect_selected;
static bool s_bt_runtime_borrowed_discovery;
static uint32_t s_bt_runtime_last_gen;
static uint64_t s_bt_runtime_started;
static uint8_t s_bt_runtime_selected_addr[6];
static Fat32State s_bt_runtime_fs;

// ---------------------------------------------------------------------------
// Address formatting
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Screen drawing
// ---------------------------------------------------------------------------

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
    bt_fill_rect_pumped(0, 0, W, lui_title_h, COL_TITLE_BG);
    fb_puts_centred(0, W, (lui_title_h - lui_char) / 2u,
                    "BLUETOOTH SCAN  --  put your devices in pairing mode",
                    COL_TEXT, COL_TITLE_BG);

    bt_fill_rect_pumped(0, H - lui_status_h, W, lui_status_h, COL_STATUS_BG);
    /* status bar: left = build tag, right = key hint */
    fb_puts(lui_margin_x, H - lui_status_h + (lui_status_h - lui_char) / 2u,
            build_tag, COL_TEXT, COL_STATUS_BG);
    fb_puts_centred(0, W, H - lui_status_h + (lui_status_h - lui_char) / 2u,
                    s_bt_runtime_active
                        ? "mouse=auto-connect  ENTER=select  ESC=cancel"
                        : "UP/DOWN=select  ENTER=pair/unpair  ESC=boot",
                    COL_TEXT, COL_STATUS_BG);
}

static unsigned s_bt_cursor; /* selected row index in scan list */

bool btscan_has_saved_mouse(void)
{
    for (unsigned i = 0u; i < bt_pairs_count(); i++) {
        const BTPair *pair = bt_pairs_get(i);
        if (pair && pair->device_type == BT_PAIRS_TYPE_MOUSE)
            return true;
    }
    return false;
}

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
    uint32_t y = lui_title_h + 4u;
    char line[64];
    unsigned p;

    p = 0u;
    for (const char *s = bt_scan_status(); *s && p < sizeof(line) - 1u; s++)
        line[p++] = *s;
    while (p < sizeof(line) - 1u) line[p++] = ' ';
    line[p] = '\0';
    fb_puts(lui_margin_x, y, line, COL_CURSOR_BG, COL_BG);
    bellatrix_launcher_pump_bt();
    y += lui_row_h + lui_row_h / 2u;

    /* Saved pairs not yet found by inquiry — show immediately on boot */
    unsigned pair_count = bt_pairs_count();
    for (unsigned i = 0u; i < pair_count; i++) {
        const BTPair *pair = bt_pairs_get(i);
        if (!pair || scan_has_addr(pair->addr)) continue;
        if (y + lui_row_h + lui_status_h >= H) break;
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
        fb_puts(lui_margin_x, y, line, COL_STATUS_BG, COL_BG);
        bellatrix_launcher_pump_bt();
        y += lui_row_h;
    }

    unsigned count = bt_scan_count();

    for (unsigned i = 0u; i < count || i == 0u; i++) {
        const BTScanResult *r = (count > 0u) ? bt_scan_get(i) : NULL;
        if (count > 0u && !r) break;
        if (y + lui_row_h + lui_status_h >= H) break;

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
        fb_puts(lui_margin_x, y, line, fg, bg);
        bellatrix_launcher_pump_bt();
        y += lui_row_h;
    }
}

// ---------------------------------------------------------------------------
// SD persistence — BTSCAN.TXT report, BTPAIRS.TXT, BTKEYS.TXT
// ---------------------------------------------------------------------------

// Append a string to a bounded text buffer; returns new length.
static uint32_t txt_append(char *buf, uint32_t len, uint32_t cap, const char *s)
{
    while (*s && len + 1u < cap) buf[len++] = *s++;
    buf[len] = '\0';
    return len;
}

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

void launcher_save_bt_report(void) { bt_save_report_to_sd(); }

/* Load BTPAIRS.TXT and BTKEYS.TXT from SD.  Reuses s_bt_report as read buffer. */
static void bt_pairs_load_from_sd(Fat32State *fs)
{
    Fat32File f;
    if (!fat32_open(fs, BT_PAIRS_FILENAME, &f)) {
        bt_pairs_load(NULL, 0u);
        kprintf("[BT] %s not found on SD FAT; no saved pairs loaded\n",
                BT_PAIRS_FILENAME);
    } else {
        uint32_t got = fat32_read(&f, s_bt_report, BT_REPORT_CAP - 1u);
        s_bt_report[got] = '\0';
        bt_pairs_load(s_bt_report, got);
        kprintf("[BT] %s read=%u bytes parsed=%u\n", BT_PAIRS_FILENAME,
                (unsigned)got, (unsigned)bt_pairs_count());
        for (unsigned i = 0u; i < bt_pairs_count(); i++) {
            const BTPair *pair = bt_pairs_get(i);
            char addr[18];
            if (!pair)
                continue;
            bt_format_addr(addr, pair->addr);
            kprintf("[BT] saved pair[%u] type=%u addr=%s name=%s\n",
                    i, (unsigned)pair->device_type, addr,
                    pair->name[0] ? pair->name : "(none)");
        }
    }

    static char s_keys_buf[BT_LINK_KEY_FILE_CAP];
    Fat32File kf;
    if (!fat32_open(fs, BT_LINK_KEY_FILENAME, &kf)) {
        bt_link_key_db_sd_load(NULL, 0u);
        kprintf("[BT] %s not found on SD FAT; no link keys loaded\n",
                BT_LINK_KEY_FILENAME);
    } else {
        uint32_t got = fat32_read(&kf, s_keys_buf, BT_LINK_KEY_FILE_CAP - 1u);
        s_keys_buf[got] = '\0';
        bt_link_key_db_sd_load(s_keys_buf, got);
        kprintf("[BT] %s read=%u bytes\n", BT_LINK_KEY_FILENAME,
                (unsigned)got);
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

// ---------------------------------------------------------------------------
// Scan screen
// ---------------------------------------------------------------------------

bool btscan_runtime_open(void)
{
    if (s_bt_runtime_active)
        return false;

    s_bt_runtime_active = true;
    s_bt_runtime_working = bellatrix_launcher_bt_ready() != 0;
    s_bt_runtime_connect_selected = false;
    s_bt_runtime_borrowed_discovery = false;
    memset(s_bt_runtime_selected_addr, 0,
           sizeof(s_bt_runtime_selected_addr));
    s_bt_cursor = 0u;
    s_bt_runtime_last_gen = 0xFFFFFFFFu;
    s_bt_runtime_started = PAL_Time_ReadCounter();

    /* Runtime reopen uses the in-memory pair/key databases already owned by
     * BT. Mount SD only as the persistence target; reloading here could erase
     * a link key learned after boot. */
    s_bt_runtime_sd_ok = bcm_emmc_init() &&
        fat32_init_with_reader(&s_bt_runtime_fs, sd_read_block_cb, NULL);
    if (s_bt_runtime_sd_ok)
        fat32_set_writer(&s_bt_runtime_fs, sd_write_block_cb, NULL);

    if (s_bt_runtime_working) {
        s_bt_runtime_borrowed_discovery =
            bellatrix_launcher_bt_recovery_discovery_active() != 0;
        bellatrix_launcher_bt_suspend_reconnect(1);
        if (!s_bt_runtime_borrowed_discovery) {
            bellatrix_launcher_bt_open_pairing();
            bt_scan_start();
        } else {
            bt_diag_log("[BT] F11 borrowed active recovery discovery\n");
        }
    }

    bt_draw_scan_chrome();
    if (!s_bt_runtime_working)
        fb_puts(lui_margin_x, lui_title_h + 4u,
                "controller unavailable - ESC to close",
                COL_CURSOR_BG, COL_BG);
    bt_draw_scan_rows();
    return true;
}

void btscan_runtime_close(bool confirmed)
{
    if (!s_bt_runtime_active)
        return;

    if (s_bt_runtime_working &&
        (!s_bt_runtime_borrowed_discovery || confirmed)) {
        if (s_bt_runtime_borrowed_discovery)
            bellatrix_launcher_bt_claim_recovery_discovery();
        bt_scan_stop();
    }

    if (confirmed && s_bt_runtime_connect_selected) {
        /* Pairing and connection outlive the screen. The reactor advances
         * authentication after the framebuffer modal has closed. */
        bellatrix_launcher_bt_prepare_pairing(
            s_bt_runtime_selected_addr);
        bellatrix_launcher_bt_suspend_reconnect(0);
        /* This function runs after the normal BTstack pass, not in its HID
 * callback. Publish reconnect immediately; the host reactor submits it on the
         * next normal reactor pass. */
        bellatrix_launcher_bt_connect_now();
    } else if (!s_bt_runtime_borrowed_discovery) {
        bellatrix_launcher_bt_suspend_reconnect(0);
        bellatrix_launcher_bt_close_pairing();
    } else {
        /* Esc returns a borrowed manager-owned scan unchanged. A confirmed
         * non-connect action claimed it above, so close that pairing window. */
        bellatrix_launcher_bt_suspend_reconnect(0);
        if (confirmed)
            bellatrix_launcher_bt_close_pairing();
    }

    if (s_bt_runtime_sd_ok)
        bt_pairs_save_to_sd(&s_bt_runtime_fs);

    s_bt_runtime_active = false;
}

void btscan_runtime_background_step(void)
{
    /* A new link key is produced asynchronously after the modal closes.
     * Persist it only once the HID link is live, from the reactor owner. */
    if (s_bt_runtime_sd_ok &&
        bellatrix_launcher_bt_mouse_connected() &&
        bt_link_key_db_sd_dirty())
        bt_pairs_save_to_sd(&s_bt_runtime_fs);
}

bool btscan_runtime_step(void)
{
    if (!s_bt_runtime_active)
        return false;

    uint8_t key = launcher_input_pop();
    bool redraw = false;

    if (key == LAUNCHER_KEY_ESC) {
        btscan_runtime_close(false);
        return false;
    }

    if (key == LAUNCHER_KEY_ENTER || key == LAUNCHER_KEY_KPENTER) {
        unsigned count = bt_scan_count();
        if (count > 0u) {
            const BTScanResult *r = bt_scan_get(s_bt_cursor);
            if (r) {
                if (bt_pairs_is_known(r->addr)) {
                    bt_pairs_remove(r->addr);
                } else {
                    (void)bt_pairs_add(r);
                    memcpy(s_bt_runtime_selected_addr, r->addr,
                           sizeof(s_bt_runtime_selected_addr));
                    s_bt_runtime_connect_selected = true;
                }
            }
        }
        btscan_runtime_close(true);
        return false;
    }

    if ((key == LAUNCHER_KEY_DEL || key == LAUNCHER_KEY_BKSP) &&
        bt_scan_count() > 0u) {
        const BTScanResult *r = bt_scan_get(s_bt_cursor);
        if (r && bt_pairs_is_known(r->addr)) {
            bt_pairs_remove(r->addr);
            redraw = true;
        }
    }

    if (key == LAUNCHER_KEY_UP && s_bt_cursor > 0u) {
        s_bt_cursor--;
        redraw = true;
    }
    if (key == LAUNCHER_KEY_DOWN &&
        s_bt_cursor + 1u < bt_scan_count()) {
        s_bt_cursor++;
        redraw = true;
    }

    uint32_t generation = bt_scan_generation();
    if (generation != s_bt_runtime_last_gen) {
        s_bt_runtime_last_gen = generation;
        unsigned count = bt_scan_count();
        if (count > 0u && s_bt_cursor >= count)
            s_bt_cursor = count - 1u;

        /* F11 is an explicit pairing operation. As soon as inquiry identifies
         * a HID mouse, stop discovery and request the observed immediate
         * connection; waiting for ENTER loses the short pairing window. */
        for (unsigned i = 0u; i < count; i++) {
            const BTScanResult *r = bt_scan_get(i);
            if (!r || !r->hid ||
                bt_pairs_classify(r) != BT_PAIRS_TYPE_MOUSE)
                continue;
            s_bt_cursor = i;
            if (!bt_pairs_is_known(r->addr))
                (void)bt_pairs_add(r);
            memcpy(s_bt_runtime_selected_addr, r->addr,
                   sizeof(s_bt_runtime_selected_addr));
            s_bt_runtime_connect_selected = true;
            bt_diag_log("[BT] F11 discovered pairing mouse %s; "
                        "connecting immediately\n",
                        r->name[0] ? r->name : "(no name)");
            btscan_runtime_close(true);
            return false;
        }
        redraw = true;
    }

    if (redraw)
        bt_draw_scan_rows();

    uint32_t timeout_ms = s_bt_runtime_working ? 90000u : 12000u;
    if (launcher_ms_since(s_bt_runtime_started) >= timeout_ms) {
        btscan_runtime_close(false);
        return false;
    }
    return true;
}

// Run the scan screen until the user continues or ~90 s elapse.
// Shown even when controller bootstrap failed so the failure remains visible
// on the framebuffer as well as the independent mini-UART log.
void btscan_screen(bool force_scan)
{
    bool working = bellatrix_launcher_bt_ready() != 0;

    if (working)
        bellatrix_launcher_bt_suspend_reconnect(1);

    /* Mount SD early so we can load BTPAIRS.TXT before the scan starts. */
    static Fat32State s_sd_fs_scan;
    bool sd_ok = bcm_emmc_init() &&
                 fat32_init_with_reader(&s_sd_fs_scan, sd_read_block_cb, NULL);
    if (sd_ok) {
        fat32_set_writer(&s_sd_fs_scan, sd_write_block_cb, NULL);
        s_bt_runtime_fs = s_sd_fs_scan;
        s_bt_runtime_sd_ok = true;
        bt_pairs_load_from_sd(&s_sd_fs_scan);
    }

    if (!force_scan && working && btscan_has_saved_mouse()) {
        bt_diag_log("[BT] saved mouse present; skipping discovery and connecting directly\n");
        bellatrix_launcher_bt_suspend_reconnect(0);
        return;
    }

    s_bt_cursor = 0u;

    if (working) {
        bellatrix_launcher_bt_open_pairing();
        bt_scan_start();
    }

    bt_draw_scan_chrome();
    if (!working)
        fb_puts(lui_margin_x, lui_title_h + 4u,
                "controller bootstrap FAILED - inspect mini-UART log",
                COL_CURSOR_BG, COL_BG);

    uint32_t last_gen = working ? 0xFFFFFFFFu : bt_scan_generation();
    // Outer iteration ≈ 0.125 ms — tight cadence keeps the 16-byte PL011
    // FIFO drained during LE advert floods (~10KB/s at 115200).
    uint32_t budget   = working ? 720000u : 96000u;   // ≈90 s / ≈12 s
    bool needs_redraw = false;
    bool mouse_selected = false;
    uint8_t selected_mouse_addr[6] = {0};

    for (uint32_t iter = 0u; iter < budget && !mouse_selected; iter++) {
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

            /* Product path: automatically select the first discoverable HID
             * mouse. Result ordering changes during inquiry, so requiring the
             * user to chase it with the keyboard saved the wrong device in
             * the hardware test that motivated ISSUE-0054. */
            if (force_scan || !btscan_has_saved_mouse()) {
                for (unsigned i = 0u; i < count; i++) {
                    const BTScanResult *r = bt_scan_get(i);
                    if (!r || !r->hid ||
                        bt_pairs_classify(r) != BT_PAIRS_TYPE_MOUSE)
                        continue;
                    s_bt_cursor = i;
                    if (!bt_pairs_is_known(r->addr))
                        (void)bt_pairs_add(r);
                    memcpy(selected_mouse_addr, r->addr,
                           sizeof(selected_mouse_addr));
                    bt_diag_log("[BT] auto-selected discovered HID mouse %s\n",
                                r->name[0] ? r->name : "(no name)");
                    mouse_selected = true;
                    break;
                }
            }
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
    }

    if (working && mouse_selected) {
        /* The observed working behavior is immediate connection on discovery.
         * Keep the explicit pairing window and page before FAT/media work. */
        bellatrix_launcher_bt_prepare_pairing(selected_mouse_addr);
        bellatrix_launcher_bt_suspend_reconnect(0);
        bellatrix_launcher_bt_connect_now();
        uint64_t connect_started = PAL_Time_ReadCounter();
        while (!bellatrix_launcher_bt_mouse_connected() &&
               launcher_ms_since(connect_started) < 5000u)
            bellatrix_launcher_pump_bt();
        bt_diag_log("[BT] immediate pairing connect ended: connected=%u "
                    "elapsed=%u ms\n",
                    bellatrix_launcher_bt_mouse_connected() ? 1u : 0u,
                    (unsigned)launcher_ms_since(connect_started));
    } else if (working) {
        bellatrix_launcher_bt_suspend_reconnect(0);
    }

    if (sd_ok)
        bt_pairs_save_to_sd(&s_sd_fs_scan);

}

#endif // BELLATRIX_ENABLE_BTSTACK
