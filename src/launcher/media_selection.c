// src/launcher/media_selection.c
// Media enumeration, selection UI and attach for the launcher. See
// media_selection.h.

#include "launcher/media_selection.h"
#include "launcher/launcher_ui.h"
#include "launcher/launcher_input.h"
#include "storage/fat/fat32.h"
#include "storage/iso/iso_image.h"
#include "host/pal.h"
#include "runtime/core_chipset.h"
#if BELLATRIX_ENABLE_USBSTACK
#include "io/usb/usb_msc_bellatrix.h"
#include "runtime/runtime.h"
#endif

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// Machine media attach entry points.
int bellatrix_machine_insert_df_adf(unsigned drive, const uint8_t *adf,
                                    uint32_t adf_size);
int bellatrix_machine_insert_df0_adf(const uint8_t *adf, uint32_t adf_size);
int bellatrix_machine_insert_iso(const void *data, size_t size);
int bellatrix_machine_attach_iso_fn(iso_read_fn fn, void *ctx, uint32_t sector_count);
int bellatrix_machine_attach_hdf_fn(int (*read_fn)(void *ctx, uint32_t lba, uint32_t count,
                                                   uint8_t *buf),
                                    void *ctx, uint32_t sector_count);

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MAX_FILES          96u
#define MAX_ADF_SELECTIONS 4u
#define ADF_BUF_SIZE       (1024u * 1024u)   // 1 MB — large enough for any DD/HD ADF

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

typedef enum RuntimeMediaPhase {
    RUNTIME_MEDIA_CLOSED = 0,
    RUNTIME_MEDIA_WAIT_MSC,
    RUNTIME_MEDIA_SELECT,
    RUNTIME_MEDIA_EMPTY,
    RUNTIME_MEDIA_APPLY_OPEN,
    RUNTIME_MEDIA_APPLY_READ,
    RUNTIME_MEDIA_APPLY_COMMIT,
    RUNTIME_MEDIA_ERROR,
} RuntimeMediaPhase;

static RuntimeMediaPhase s_runtime_phase;
static MediaEntry s_runtime_entries[MAX_FILES];
static uint32_t s_runtime_count;
static uint32_t s_runtime_cursor;
static uint32_t s_runtime_scroll;
static uint64_t s_runtime_started;
static uint32_t s_runtime_apply_index;
static uint32_t s_runtime_apply_drive;
#if BELLATRIX_ENABLE_USBSTACK
static uint32_t s_runtime_loaded;
static char s_runtime_names[MAX_FILES][FAT32_NAME_MAX];
static Fat32File s_runtime_adf_file;
#endif

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
// Media list screen
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
    fb_fill_rect(0, 0, W, lui_title_h, COL_TITLE_BG);
    fb_puts_centred(0, W, (lui_title_h - lui_char) / 2u,
                    s_runtime_phase != RUNTIME_MEDIA_CLOSED
                        ? "BELLATRIX MEDIA  --  Select up to 4 ADFs"
                        : "BELLATRIX LAUNCHER  --  Select media (ISO, HDF, up to 4 ADF):",
                    COL_TEXT, COL_TITLE_BG);

    // File list
    uint32_t list_y = lui_title_h + 4u;

    for (uint32_t i = 0u; i < lui_visible_rows && (scroll + i) < count; i++) {
        uint32_t         idx      = scroll + i;
        const MediaEntry *e       = &entries[idx];
        bool              selected = (idx == cursor);

        uint16_t bg  = selected ? COL_CURSOR_BG : COL_BG;
        uint16_t fg  = selected ? COL_TEXT_SEL  : COL_TEXT;

        uint32_t row_y = list_y + i * lui_row_h;

        fb_fill_rect(0, row_y, W, lui_row_h, bg);

        if (selected) {
            fb_putchar(lui_margin_x - lui_char - 2u, row_y + 2u, '>', fg, bg);
        }

        char mark[5] = { '[', e->selected ? '*' : ' ', ']', ' ', '\0' };
        uint32_t x = fb_puts(lui_margin_x, row_y + 2u, mark, fg, bg);

        // Type tag
        const char *tag = media_tag(e->type);
        x = fb_puts(x, row_y + 2u, tag, COL_HINT, bg);
        fb_puts(x, row_y + 2u, e->name, fg, bg);
    }

    // Status bar
    uint32_t status_y = H - lui_status_h;
    fb_fill_rect(0, status_y, W, lui_status_h, COL_STATUS_BG);

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
    const char *suffix = s_runtime_phase != RUNTIME_MEDIA_CLOSED
        ? "  |  UP/DOWN: navigate  |  SPACE: mark  |  ENTER: insert/close  |  ESC: cancel"
        : "  |  UP/DOWN: navigate  |  SPACE: mark  |  ENTER: boot  |  ESC: no disk";
    for (const char *p = suffix; *p; p++) hint[n_chars++] = *p;
    hint[n_chars] = '\0';

    fb_puts_centred(0, W, status_y + (lui_status_h - lui_char) / 2u,
                    hint, COL_TEXT, COL_STATUS_BG);
}

// ---------------------------------------------------------------------------
// ADF buffer + FAT32 state — kept alive for the duration of emulation so the
// ATAPI/IDE read callbacks can seek and read sectors on demand.
// ---------------------------------------------------------------------------

static uint8_t s_adf_buf[MAX_ADF_SELECTIONS][ADF_BUF_SIZE] __attribute__((aligned(512)));
#if BELLATRIX_ENABLE_USBSTACK
static uint8_t s_adf_stage[ADF_BUF_SIZE] __attribute__((aligned(512)));
#endif

#if BELLATRIX_ENABLE_USBSTACK
static Fat32State s_fat32;
static Fat32File  s_iso_file;
static Fat32File  s_hdf_file;

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

#if BELLATRIX_ENABLE_USBSTACK
static uint32_t media_runtime_scan_adfs(void)
{
    s_runtime_count = 0u;
    if (!usb_msc_is_ready() || !fat32_init_usb(&s_fat32))
        return 0u;

    uint32_t n_files = fat32_list(&s_fat32, s_runtime_names, MAX_FILES);
    for (uint32_t i = 0u; i < n_files && s_runtime_count < MAX_FILES; i++) {
        if (!name_has_ext(s_runtime_names[i], ".adf"))
            continue;
        MediaEntry *entry = &s_runtime_entries[s_runtime_count++];
        memcpy(entry->name, s_runtime_names[i], FAT32_NAME_MAX);
        entry->type = MEDIA_ADF;
        entry->selected = false;
    }
    sort_media_entries(s_runtime_entries, s_runtime_count);
    kprintf("[LAUNCHER] runtime media: %u files, %u ADFs\n",
            (unsigned)n_files, (unsigned)s_runtime_count);
    return s_runtime_count;
}
#endif

bool media_selection_runtime_open(void)
{
    if (s_runtime_phase != RUNTIME_MEDIA_CLOSED)
        return false;

    s_runtime_count = 0u;
    s_runtime_cursor = 0u;
    s_runtime_scroll = 0u;
    s_runtime_started = PAL_Time_ReadCounter();
    while (launcher_input_pop() != 0u) {}
#if BELLATRIX_ENABLE_USBSTACK
    if (!usb_msc_is_ready()) {
        s_runtime_phase = RUNTIME_MEDIA_EMPTY;
        draw_message("USB media unavailable. ENTER/ESC closes.",
                     COL_STATUS_BG);
        kprintf("[LAUNCHER] F12: no USB mass-storage device available\n");
        return true;
    }
#endif
    s_runtime_phase = RUNTIME_MEDIA_WAIT_MSC;
    draw_message("Scanning USB media...", COL_TITLE_BG);
    return true;
}

void media_selection_runtime_close(void)
{
    s_runtime_phase = RUNTIME_MEDIA_CLOSED;
}

static void media_runtime_fail(const char *message)
{
    kprintf("[LAUNCHER] runtime media failed: %s\n", message);
    draw_message(message, COL_STATUS_BG);
    s_runtime_phase = RUNTIME_MEDIA_ERROR;
}

bool media_selection_runtime_step(void)
{
    if (s_runtime_phase == RUNTIME_MEDIA_CLOSED)
        return false;

    uint8_t key = launcher_input_pop();
    if (key == LAUNCHER_KEY_ESC) {
        media_selection_runtime_close();
        return false;
    }

    if (s_runtime_phase == RUNTIME_MEDIA_WAIT_MSC) {
#if BELLATRIX_ENABLE_USBSTACK
        if (usb_msc_is_ready()) {
            if (media_runtime_scan_adfs() != 0u) {
                s_runtime_phase = RUNTIME_MEDIA_SELECT;
                draw_frame(s_runtime_count, s_runtime_cursor,
                           s_runtime_scroll, s_runtime_entries);
            } else {
                s_runtime_phase = RUNTIME_MEDIA_EMPTY;
                draw_message("No ADF media found. ENTER/ESC closes.",
                             COL_STATUS_BG);
            }
        } else
#endif
        if (launcher_ms_since(s_runtime_started) >= 5000u) {
            s_runtime_phase = RUNTIME_MEDIA_EMPTY;
            draw_message("USB media unavailable. ENTER/ESC closes.",
                         COL_STATUS_BG);
        }
        return true;
    }

    if (s_runtime_phase == RUNTIME_MEDIA_EMPTY ||
        s_runtime_phase == RUNTIME_MEDIA_ERROR) {
        if (key == LAUNCHER_KEY_ENTER || key == LAUNCHER_KEY_KPENTER) {
            media_selection_runtime_close();
            return false;
        }
        return true;
    }

    if (s_runtime_phase == RUNTIME_MEDIA_SELECT) {
        bool redraw = false;
        if (key == LAUNCHER_KEY_UP && s_runtime_cursor > 0u) {
            s_runtime_cursor--;
            if (s_runtime_cursor < s_runtime_scroll)
                s_runtime_scroll = s_runtime_cursor;
            redraw = true;
        } else if (key == LAUNCHER_KEY_DOWN &&
                   s_runtime_cursor + 1u < s_runtime_count) {
            s_runtime_cursor++;
            if (s_runtime_cursor >= s_runtime_scroll + lui_visible_rows)
                s_runtime_scroll = s_runtime_cursor - lui_visible_rows + 1u;
            redraw = true;
        } else if (key == LAUNCHER_KEY_SPACE) {
            toggle_media_selection(s_runtime_entries, s_runtime_count,
                                   s_runtime_cursor);
            redraw = true;
        } else if (key == LAUNCHER_KEY_ENTER ||
                   key == LAUNCHER_KEY_KPENTER) {
            if (!any_media_selected(s_runtime_entries, s_runtime_count))
                toggle_media_selection(s_runtime_entries, s_runtime_count,
                                       s_runtime_cursor);
            s_runtime_apply_index = 0u;
            s_runtime_apply_drive = 0u;
            s_runtime_phase = RUNTIME_MEDIA_APPLY_OPEN;
            draw_message("Loading selected ADF...", COL_TITLE_BG);
        }
        if (redraw)
            draw_frame(s_runtime_count, s_runtime_cursor,
                       s_runtime_scroll, s_runtime_entries);
        return true;
    }

#if BELLATRIX_ENABLE_USBSTACK
    if (s_runtime_phase == RUNTIME_MEDIA_APPLY_OPEN) {
        while (s_runtime_apply_index < s_runtime_count &&
               !s_runtime_entries[s_runtime_apply_index].selected)
            s_runtime_apply_index++;
        if (s_runtime_apply_index >= s_runtime_count) {
            media_selection_runtime_close();
            return false;
        }

        const MediaEntry *entry =
            &s_runtime_entries[s_runtime_apply_index];
        if (!fat32_open(&s_fat32, entry->name, &s_runtime_adf_file) ||
            s_runtime_adf_file.file_size == 0u ||
            s_runtime_adf_file.file_size > ADF_BUF_SIZE) {
            media_runtime_fail("ADF open/size failed. ENTER/ESC closes.");
            return true;
        }
        s_runtime_loaded = 0u;
        s_runtime_phase = RUNTIME_MEDIA_APPLY_READ;
        return true;
    }

    if (s_runtime_phase == RUNTIME_MEDIA_APPLY_READ) {
        uint32_t remaining = s_runtime_adf_file.file_size - s_runtime_loaded;
        uint32_t chunk = remaining > 512u ? 512u : remaining;
        uint32_t got = fat32_read(&s_runtime_adf_file,
                                  s_adf_stage + s_runtime_loaded, chunk);
        if (got != chunk) {
            media_runtime_fail("ADF read failed. ENTER/ESC closes.");
            return true;
        }
        s_runtime_loaded += got;
        if (s_runtime_loaded == s_runtime_adf_file.file_size)
            s_runtime_phase = RUNTIME_MEDIA_APPLY_COMMIT;
        return true;
    }

    if (s_runtime_phase == RUNTIME_MEDIA_APPLY_COMMIT) {
        core_chipset_lock_acquire();
        memcpy(s_adf_buf[s_runtime_apply_drive], s_adf_stage,
               s_runtime_loaded);
        int inserted = bellatrix_machine_insert_df_adf(
            s_runtime_apply_drive, s_adf_buf[s_runtime_apply_drive],
            s_runtime_loaded);
        core_chipset_lock_release();
        if (!inserted) {
            media_runtime_fail("ADF insert failed. ENTER/ESC closes.");
            return true;
        }
        s_runtime_apply_drive++;
        s_runtime_apply_index++;
        s_runtime_phase = RUNTIME_MEDIA_APPLY_OPEN;
        return true;
    }
#else
    media_runtime_fail("USB support disabled. ENTER/ESC closes.");
#endif
    return true;
}

// ---------------------------------------------------------------------------
// QEMU fallback: load media placed by "-device loader,addr=..."
//
// When running under QEMU with no SD card, the host tool injects images at
// fixed physical addresses via the QEMU generic loader device.  Emu68's linear
// kernel map places physical P at virtual (0xffffff9000000000 + P).
//
// On real hardware, random RAM at those addresses won't have the magic, so
// these are always no-ops on bare metal.
// ---------------------------------------------------------------------------

#define QEMU_ADF_PHYS    0x18000000UL
#define QEMU_ADF_KVIRT   ((const uint8_t *)(0xffffff9000000000ULL + QEMU_ADF_PHYS))
#define QEMU_ISO_PHYS    0x20000000UL
#define QEMU_ISO_KVIRT   ((const uint8_t *)(0xffffff9000000000ULL + QEMU_ISO_PHYS))
#define ADF_SIZE_DD      901120u   // 80 tracks × 11 sectors × 512 bytes

bool media_selection_qemu_media_present(void)
{
    const uint8_t *adf = QEMU_ADF_KVIRT;
    const uint8_t *iso = QEMU_ISO_KVIRT;

    return (adf[0] == 'D' && adf[1] == 'O' && adf[2] == 'S') ||
           (iso[0x8000] == 0x01u &&
            iso[0x8001] == 'C' && iso[0x8002] == 'D' &&
            iso[0x8003] == '0' && iso[0x8004] == '0' &&
            iso[0x8005] == '1');
}

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
// Media selection flow
// ---------------------------------------------------------------------------

bool media_selection_run(void)
{
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

    // Phase: wait for the USB mass-storage LUN to finish enumerating before
    // scanning for media. Deterministic wall-clock budget (was a fixed nop-spin
    // that expired before slow hubs/throttled controllers finished — the drive
    // then reported ready only AFTER this check, which the deferred console
    // hid by reordering "[USB-MSC] drive ready" ahead of "no media found").
    draw_message("Scanning USB drive...", COL_TITLE_BG);
    uint64_t t_enum = PAL_Time_ReadCounter();
    bool msc_ready = false;
    while (!(msc_ready = usb_msc_is_ready()) && launcher_ms_since(t_enum) < 5000u)
        bellatrix_runtime_io_pump();
    kprintf("[LAUNCHER] USB MSC %s after %u ms\n",
            msc_ready ? "ready" : "NOT ready (5s timeout)",
            (unsigned)launcher_ms_since(t_enum));

    if (msc_ready && fat32_init_usb(&s_fat32)) {
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
        bellatrix_runtime_io_pump();

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
                if (cursor >= scroll + lui_visible_rows) scroll = cursor - lui_visible_rows + 1u;
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
