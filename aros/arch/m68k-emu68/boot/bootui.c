/*
 * Minimal boot presentation over a linear framebuffer.
 *
 * Knows nothing about the machine it runs on. The surface, the log channel and
 * the boot arguments all arrive through bootui_platform.h, so this file works
 * anywhere something can answer those three questions -- which is the point:
 * a splash that names its bootloader is that bootloader's splash, not the
 * system's.
 *
 * This deliberately has no Exec, graphics.library or font dependencies: it
 * is first called before SysBase exists.  All pixels are stored bytewise so
 * the little-endian framebuffer format is correct on the big-endian 68k.
 */

#include "bootui_platform.h"
#include "bootui_api.h"

#include <aros/bootui.h>
#include <exec/nodes.h>
#include <exec/memory.h>
#include <exec/memheaderext.h>
#include <exec/execbase.h>
#include <proto/exec.h>

#include "bootimage.inc"

#define RGB565(r, g, b) \
    ((((uint16_t)(r) & 0xf8) << 8) | (((uint16_t)(g) & 0xfc) << 3) | ((b) >> 3))

struct BootUIState
{
    uint8_t *framebuffer;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t progress;
    const char *status;
    /*
     * One integer magnification for everything drawn here.
     *
     * Every measurement below used to be an absolute pixel count chosen for a
     * 640x480 screen, which is why a real Pi at 1920x1080 showed a hairline
     * loader under unreadable text. The factor comes from both axes, not just
     * the width: 1920/640 is 3 but 1080/480 is 2, and taking the width alone
     * would scale the splash to 1440 lines on a 1080-line display.
     */
    uint32_t scale;
    uint32_t clock_start_us;
    uint32_t elapsed_seconds;
    int clock_started;
    int active;
};

static struct BootUIState bootui;

/*
 * Holding the display past the driver's hand-off.
 *
 * Without this the splash disappears the instant Intuition installs its first
 * bitmap, which is well before Wanderer has drawn anything, so the boot ends
 * on several seconds of half-built desktop. Holding is only possible because
 * fbgfx composes into each bitmap's own buffer and copies to the framebuffer
 * separately (fbDoRefreshArea): suppressing that copy leaves the splash on
 * screen while the desktop is assembled behind it, which is double buffering
 * with the bitmap as the back buffer and costs nothing extra.
 *
 * The release is deliberately split in two. The deadline is noticed in
 * bootui_clock_tick(), which runs from the system timer *interrupt*
 * (platform/bcm283x/systimer_heartbeat), and nothing there may take a
 * semaphore or call into OOP -- the first version did and Wanderer collected
 * two "called in supervisor mode" alerts for it. So the tick only stops the
 * hold and records that the screen owes a full repaint; the driver notices on
 * its next refresh, in task context, and paints there.
 *
 * The signal comes from Wanderer. IconVolumeList's first Update says the
 * backdrop window has its volume icons, which is the closest thing to "the
 * desktop exists" that anything in the system knows -- IconList has no
 * attribute or notification for it, so the signal had to be added there.
 *
 * It arms the release rather than performing it. "The icons are in the list"
 * is not yet "the icons are painted": MUI lays out on that message and draws
 * on the next redraw, so releasing there would copy a screen caught mid-draw.
 * The signal releases the hold and repaints on the spot. Two delayed variants
 * were tried and both are dead ends, for the same underlying reason -- the
 * repaint can only run from a task, and no task of ours comes past:
 *
 *   waiting for the drawing to go quiet releases exactly when nothing is left
 *   to carry the repaint;
 *
 *   waiting for the next refresh after that measured seventy seconds on a
 *   boot, because once the icons are drawn nothing draws again for a long
 *   time.
 *
 * IconVolumeList's Update has already run its super method by the time it
 * signals, so the icons are in the bitmap, and it signals from Wanderer's own
 * task. That is the one moment that is both "the desktop is ready" and "we are
 * on a task", so the work belongs there and nowhere else.
 *
 * BOOTUI_HOLD_CAP_SECONDS is not tuning, it is the failure mode. Nothing can
 * send BOOTUI_STAGE_ICONS yet, and a hold that never ends is a frozen splash
 * with a working desktop invisible behind it, so the cap always applies.
 */
#define BOOTUI_HOLD_QUIET_US    1500000UL
#define BOOTUI_HOLD_CAP_SECONDS 30

static int bootui_release_pending;
static int bootui_hold_active;
static uint32_t bootui_hold_started_at;
/* Written by the timer interrupt, read by the task that arms the hold. */
static volatile uint32_t bootui_last_tick_us;
/* Set from the driver's refresh path -- task context -- every time something
 * is drawn while the hold is on. Cleared by the tick, which is how a stretch
 * with no drawing is noticed. */
static volatile int bootui_hold_drawing;
static uint32_t bootui_hold_quiet_since_us;
static int bootui_hold_quiet_valid;
static int bootui_hold_armed;
/*
 * Set when the Startup-Sequence reports it is about to run Wanderer. The hold
 * is for the screen that opens after that and for no other: anything opening a
 * screen earlier -- a requester, an alert -- has a better claim on the display
 * than a splash, and must not be covered by one.
 */
static int bootui_wanderer_started;
/*
 * Whether a presentation has already been seen. The first one is the desktop
 * handoff candidate and starts the hold; every later one ends it. Kept apart
 * from bootui_hold_active because the hold can end on its own deadline, and a
 * presentation arriving after that must not start a second one.
 */
static int bootui_holding_seen;
static int bootui_direct_scanout;
/*
 * Called to put the finished desktop up. Registered by the display driver and
 * invoked ONLY from bootui_set_stage(), which runs in the task that
 * sends the signal -- Wanderer's. It takes a semaphore and walks OOP objects,
 * neither of which is legal from the timer interrupt, and an earlier version
 * that called it from there earned two "called in supervisor mode" alerts.
 */
static void (*bootui_release_hook)(void);
static void bootui_release(void);

/*
 * Every BootUI event carries the time it happened, measured from the same
 * clock the splash shows.
 *
 * The alternative was a line per tick to prove the clock was still running,
 * which answered one question once and then buried the log. Timestamps answer
 * that one and every later one -- how long the hold lasted, how far the
 * release was from the repaint -- without a line of their own.
 */
static void bootui_event(const char *what)
{
    char stamp[16];
    uint32_t ms = (bootui_last_tick_us - bootui.clock_start_us) / 1000UL;
    uint32_t s = ms / 1000UL;
    uint32_t m = s / 60UL;

    s %= 60UL;
    ms %= 1000UL;
    if (m > 99)
        m = 99;

    stamp[0] = '[';
    stamp[1] = '0' + m / 10;  stamp[2] = '0' + m % 10;
    stamp[3] = ':';
    stamp[4] = '0' + s / 10;  stamp[5] = '0' + s % 10;
    stamp[6] = '.';
    stamp[7] = '0' + ms / 100;
    stamp[8] = '0' + (ms / 10) % 10;
    stamp[9] = '0' + ms % 10;
    stamp[10] = ']';
    stamp[11] = ' ';
    stamp[12] = 0;

    bootui_platform_log("[BootUI] ");
    bootui_platform_log(stamp);
    bootui_platform_log(what);
    bootui_platform_log("\n");
}
static struct BootUIResource bootui_resource;

static void put_pixel(uint32_t x, uint32_t y, uint16_t color);
static void fill_rect(uint32_t x, uint32_t y, uint32_t width,
                      uint32_t height, uint16_t color);

/* Complete 5x7 uppercase alphabet used by boot status strings. */
struct Glyph
{
    char chr;
    uint8_t rows[7];
};

static const struct Glyph glyphs[] =
{
    { ' ', { 0, 0, 0, 0, 0, 0, 0 } },
    { '.', { 0, 0, 0, 0, 0, 6, 6 } },
    { ':', { 0, 6, 6, 0, 6, 6, 0 } },
    { '0', { 14, 17, 19, 21, 25, 17, 14 } },
    { '1', { 4, 12, 4, 4, 4, 4, 14 } },
    { '2', { 14, 17, 1, 2, 4, 8, 31 } },
    { '3', { 30, 1, 1, 14, 1, 1, 30 } },
    { '4', { 2, 6, 10, 18, 31, 2, 2 } },
    { '5', { 31, 16, 16, 30, 1, 1, 30 } },
    { '6', { 14, 16, 16, 30, 17, 17, 14 } },
    { '7', { 31, 1, 2, 4, 8, 8, 8 } },
    { '8', { 14, 17, 17, 14, 17, 17, 14 } },
    { '9', { 14, 17, 17, 15, 1, 1, 14 } },
    { 'A', { 14, 17, 17, 31, 17, 17, 17 } },
    { 'B', { 30, 17, 17, 30, 17, 17, 30 } },
    { 'C', { 14, 17, 16, 16, 16, 17, 14 } },
    { 'D', { 30, 17, 17, 17, 17, 17, 30 } },
    { 'E', { 31, 16, 16, 30, 16, 16, 31 } },
    { 'F', { 31, 16, 16, 30, 16, 16, 16 } },
    { 'G', { 14, 17, 16, 23, 17, 17, 14 } },
    { 'H', { 17, 17, 17, 31, 17, 17, 17 } },
    { 'I', { 14, 4, 4, 4, 4, 4, 14 } },
    { 'J', { 7, 2, 2, 2, 18, 18, 12 } },
    { 'K', { 17, 18, 20, 24, 20, 18, 17 } },
    { 'L', { 16, 16, 16, 16, 16, 16, 31 } },
    { 'M', { 17, 27, 21, 21, 17, 17, 17 } },
    { 'N', { 17, 25, 21, 19, 17, 17, 17 } },
    { 'O', { 14, 17, 17, 17, 17, 17, 14 } },
    { 'P', { 30, 17, 17, 30, 16, 16, 16 } },
    { 'Q', { 14, 17, 17, 17, 21, 18, 13 } },
    { 'R', { 30, 17, 17, 30, 20, 18, 17 } },
    { 'S', { 15, 16, 16, 14, 1, 1, 30 } },
    { 'T', { 31, 4, 4, 4, 4, 4, 4 } },
    { 'U', { 17, 17, 17, 17, 17, 17, 14 } },
    { 'V', { 17, 17, 17, 17, 17, 10, 4 } },
    { 'W', { 17, 17, 17, 21, 21, 21, 10 } },
    { 'X', { 17, 17, 10, 4, 10, 17, 17 } },
    { 'Y', { 17, 17, 10, 4, 4, 4, 4 } },
    { 'Z', { 31, 1, 2, 4, 8, 16, 31 } }
};

/*
 * Paint the splash scaled and centred, whatever the mode.
 *
 * The old version required the framebuffer to be exactly 640x480 with a pitch
 * to match, and returned 0 in silence otherwise -- which is why nothing
 * appeared on a Pi. The requirement was not arbitrary: the run-length data is
 * one flat sequence of pixels for the whole image, and the loop walked it
 * straight into the framebuffer with no per-row addressing, so it was only
 * correct while a source row and a destination row were the same length.
 *
 * So the runs are now cut at source row boundaries and each piece is drawn as
 * a rectangle, which handles both the pitch and the magnification for free.
 */
static int draw_boot_image(void)
{
    uint32_t scale = bootui.scale;
    uint32_t dst_w = BOOT_IMAGE_WIDTH * scale;
    uint32_t dst_h = BOOT_IMAGE_HEIGHT * scale;
    uint32_t origin_x;
    uint32_t origin_y;
    uint32_t pos = 0;
    uint32_t run;

    if (bootui.width < dst_w || bootui.height < dst_h)
        return 0;
    origin_x = (bootui.width - dst_w) / 2;
    origin_y = (bootui.height - dst_h) / 2;

    for (run = 0; run < sizeof(boot_image_runs) / sizeof(boot_image_runs[0]); run++)
    {
        uint32_t count = boot_image_runs[run].count;
        /*
         * No byte swap here, deliberately.
         *
         * make-boot-image.py stores the true RGB565 value, and the version of
         * this loop that wrote 16-bit words had to swap it because a native
         * store on the 68k puts the high byte first. put_pixel() does not: it
         * writes the two bytes low-first, which is the framebuffer's order
         * already, so swapping as well would reverse them.
         *
         * That is reasoning about this code, not a diagnosis of the neon
         * splash seen on a Pi: a test that XORed the green channel here
         * changed nothing on a QEMU screen, so something other than this
         * function is putting the image up in that configuration and this
         * has never been shown to be what the eye was complaining about.
         */
        uint16_t colour = boot_image_runs[run].colour;

        while (count > 0)
        {
            uint32_t src_x = pos % BOOT_IMAGE_WIDTH;
            uint32_t src_y = pos / BOOT_IMAGE_WIDTH;
            uint32_t span = BOOT_IMAGE_WIDTH - src_x;

            if (span > count)
                span = count;

            fill_rect(origin_x + src_x * scale, origin_y + src_y * scale,
                      span * scale, scale, colour);

            pos += span;
            count -= span;
        }
    }
    return 1;
}

static void put_pixel(uint32_t x, uint32_t y, uint16_t color)
{
    uint8_t *pixel;

    if (x >= bootui.width || y >= bootui.height)
        return;
    pixel = bootui.framebuffer + y * bootui.pitch
          + x * (bootui.depth == 32 ? 4 : 2);
    if (bootui.depth == 32)
    {
        uint8_t r5 = (color >> 11) & 0x1f;
        uint8_t g6 = (color >> 5) & 0x3f;
        uint8_t b5 = color & 0x1f;

        /* vc4gfx requests VCPXFMT_BGR: B, G, R, padding in memory. */
        pixel[0] = (b5 << 3) | (b5 >> 2);
        pixel[1] = (g6 << 2) | (g6 >> 4);
        pixel[2] = (r5 << 3) | (r5 >> 2);
        pixel[3] = 0;
    }
    else
    {
        pixel[0] = color & 0xff;
        pixel[1] = color >> 8;
    }
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t width,
                      uint32_t height, uint16_t color)
{
    uint32_t px;
    uint32_t py;

    if (x >= bootui.width || y >= bootui.height)
        return;
    if (width > bootui.width - x)
        width = bootui.width - x;
    if (height > bootui.height - y)
        height = bootui.height - y;

    for (py = 0; py < height; py++)
        for (px = 0; px < width; px++)
            put_pixel(x + px, y + py, color);
}

static const uint8_t *find_glyph(char chr)
{
    uint32_t i;

    for (i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
        if (glyphs[i].chr == chr)
            return glyphs[i].rows;
    return glyphs[0].rows;
}

static void draw_text(const char *text, uint32_t y, uint32_t scale,
                      uint16_t color)
{
    uint32_t length = 0;
    uint32_t x;
    uint32_t row;
    uint32_t col;

    while (text[length])
        length++;
    x = (bootui.width - length * 6 * scale + scale) / 2;

    while (*text)
    {
        const uint8_t *rows = find_glyph(*text++);

        for (row = 0; row < 7; row++)
            for (col = 0; col < 5; col++)
                if (rows[row] & (1 << (4 - col)))
                    fill_rect(x + col * scale, y + row * scale,
                              scale, scale, color);
        x += 6 * scale;
    }
}

static const char *stage_status(uint32_t stage, uint32_t *progress)
{
    switch (stage)
    {
    case BOOTUI_STAGE_ENTRY:
        *progress = 2;
        return "STARTING BELLATRIX...";
    case BOOTUI_STAGE_EXEC:
        *progress = 6;
        return "STARTING EXEC...";
    case BOOTUI_STAGE_SYSTEM:
        *progress = 9;
        return "INITIALIZING SYSTEM...";
    case BOOTUI_STAGE_KERNEL:
        *progress = 12;
        return "STARTING KERNEL...";
    case BOOTUI_STAGE_COLDSTART:
        *progress = 15;
        return "LOADING SYSTEM...";
    case BOOTUI_STAGE_GRAPHICS:
        *progress = 20;
        return "STARTING GRAPHICS...";
    case BOOTUI_STAGE_DOS_READY:
        *progress = 28;
        return "STARTING DOS...";
    case BOOTUI_STAGE_STARTUP:
        *progress = 50;
        return "STARTING SERVICES...";
    case BOOTUI_STAGE_DESKTOP:
        *progress = 90;
        return "STARTING WANDERER...";
    default:
        return 0;
    }
}

static void draw_progress(uint32_t progress, const char *status)
{
    uint32_t scale = bootui.scale;
    /*
     * Half the screen wide, and that was already right -- the bar was never
     * too wide. What it was is too thin and too close to the bottom edge, so
     * the height and the margin below it are the two numbers that changed,
     * and they changed at every resolution rather than only on large ones.
     */
    uint32_t bar_width = bootui.width / 2;
    uint32_t bar_height = 10 * scale;
    uint32_t bar_x = (bootui.width - bar_width) / 2;
    uint32_t bar_y = bootui.height - 52 * scale;
    uint32_t text_y = bar_y - 28 * scale;
    uint32_t band_y = text_y - 8 * scale;
    uint32_t band_height = bar_y + bar_height + 8 * scale - band_y;
    uint16_t background = RGB565(0, 0, 0);
    uint16_t track = RGB565(29, 31, 38);
    uint16_t accent = RGB565(116, 83, 234);

    fill_rect(0, band_y, bootui.width, band_height, background);
    draw_text(status, text_y, scale, RGB565(190, 193, 203));
    fill_rect(bar_x, bar_y, bar_width, bar_height, track);
    fill_rect(bar_x, bar_y, bar_width * progress / 100, bar_height, accent);
}

static void draw_clock(void)
{
    char text[6];
    uint32_t scale = bootui.scale;
    uint32_t seconds = bootui.elapsed_seconds;
    uint32_t minutes;

    if (seconds > 99 * 60 + 59)
        seconds = 99 * 60 + 59;
    minutes = seconds / 60;
    seconds %= 60;
    text[0] = '0' + minutes / 10;
    text[1] = '0' + minutes % 10;
    text[2] = ':';
    text[3] = '0' + seconds / 10;
    text[4] = '0' + seconds % 10;
    text[5] = 0;

    fill_rect(0, bootui.height - 16 * scale, bootui.width, 12 * scale,
              RGB565(0, 0, 0));
    draw_text(text, bootui.height - 15 * scale, scale, RGB565(116, 83, 234));
}

void bootui_init(void)
{
    void *framebuffer = NULL;
    uint32_t pitch = 0, width = 0, height = 0, depth = 0;
    /*
     * The same black the artwork and the progress band use.
     *
     * This was RGB565(7, 12, 24) -- a near-black with a blue cast, which is a
     * perfectly good colour and the wrong one here. The boot image's own
     * ground is 0x0000 (see bootimage.inc) and draw_progress() clears its band
     * to RGB565(0, 0, 0), so a bluish ground drew a visible rectangle around
     * both: three regions, two blacks, and seams where they met.
     *
     * Any single value would remove the seams; black is the one that needs no
     * change to the artwork.
     */
    uint16_t background = RGB565(0, 0, 0);

    bootui.active = 0;
    if (!bootui_platform_surface(&framebuffer, &pitch, &width, &height, &depth))
        return;
    if (!framebuffer || !pitch || !width || !height || pitch < width * 2)
        return;

    bootui.framebuffer = framebuffer;
    bootui.pitch = pitch;
    bootui.width = width;
    bootui.height = height;
    bootui.depth = depth ? depth : 16;

    {
        uint32_t by_width = bootui.width / BOOT_IMAGE_WIDTH;
        uint32_t by_height = bootui.height / BOOT_IMAGE_HEIGHT;

        bootui.scale = by_width < by_height ? by_width : by_height;
        if (bootui.scale < 1)
            bootui.scale = 1;
        if (bootui.scale > 3)
            bootui.scale = 3;
    }

    bootui.progress = 0;
    bootui.status = NULL;
    bootui.clock_start_us = 0;
    bootui.elapsed_seconds = 0;
    bootui.clock_started = 0;
    bootui.active = 1;

    /*
     * Ground first, then the splash on top of it. Centred art does not cover
     * the screen, and what it does not cover is whatever the firmware left
     * there.
     */
    fill_rect(0, 0, bootui.width, bootui.height, background);
    if (!draw_boot_image())
        bootui_platform_log("[BootUI] framebuffer too small for "
                           "the boot image\n");
}

void bootui_retarget(void *framebuffer, uint32_t pitch,
                           uint32_t width, uint32_t height, uint32_t depth)
{
    uint32_t bytes_per_pixel;

    if (!bootui.active)
    {
        bootui_platform_log("[BootUI] retarget ignored (inactive)\n");
        return;
    }
    if (!framebuffer || !width || !height)
    {
        bootui_event("retarget rejected: incomplete geometry");
        return;
    }
    if (depth != 16 && depth != 32)
    {
        bootui_event("retarget rejected: unsupported depth");
        return;
    }

    bytes_per_pixel = depth / 8;
    if (pitch < width * bytes_per_pixel)
    {
        bootui_event("retarget rejected: short pitch");
        return;
    }

    bootui.framebuffer = framebuffer;
    bootui.pitch = pitch;
    bootui.width = width;
    bootui.height = height;
    bootui.depth = depth;
    bootui_direct_scanout = 1;

    {
        uint32_t by_width = width / BOOT_IMAGE_WIDTH;
        uint32_t by_height = height / BOOT_IMAGE_HEIGHT;

        bootui.scale = by_width < by_height ? by_width : by_height;
        if (bootui.scale < 1)
            bootui.scale = 1;
        if (bootui.scale > 3)
            bootui.scale = 3;
    }

    /* The mode change that opens Wanderer's screen is the hand-off point for
     * a direct-scanout driver. Unlike fbgfx, vc4gfx has no private desktop
     * buffer to build behind the splash: keeping the BootUI active here would
     * make its timer repaint the clock over the live Workbench framebuffer. */
    if (bootui_wanderer_started)
    {
        fill_rect(0, 0, width, height, RGB565(0, 0, 0));
        bootui_event("display takeover: vc4 mode");
        bootui.active = 0;
        return;
    }

    fill_rect(0, 0, width, height, RGB565(0, 0, 0));
    draw_boot_image();
    if (bootui.status)
    {
        draw_progress(bootui.progress, bootui.status);
        draw_clock();
    }
    bootui_event(depth == 32 ? "retargeted to RGB32 framebuffer"
                             : "retargeted to RGB16 framebuffer");
}

/*
 * Walk the heap at every stage boundary, when asked to.
 *
 * ISSUE-0037 is a block header that gets overwritten: by the time anything
 * frees the damaged block the writer is long gone, and mungwall cannot help
 * because its walls move every allocation and the corruption does not survive
 * the move. A walk of the chain the allocator already maintains changes no
 * layout at all, so running it at each boot stage brackets the damage between
 * two stages with the heap exactly as it is when the defect happens.
 *
 * Off unless the boot arguments say `heapscan`, because it is O(blocks) and
 * this port is being measured.
 */
static int bootui_heapscan = -1;

static int heapscan_wanted(void)
{
    uint32_t len = 0;
    const char *args = bootui_platform_args(&len);
    uint32_t i;

    if (!args || len < 8)
        return 0;

    for (i = 0; i + 8 <= len; i++)
    {
        if (args[i] == 'h' && args[i + 1] == 'e' && args[i + 2] == 'a'
            && args[i + 3] == 'p' && args[i + 4] == 's' && args[i + 5] == 'c'
            && args[i + 6] == 'a' && args[i + 7] == 'n')
            return 1;
    }

    return 0;
}

/* kernel.resource's TLSF walker; kernel_resource.o keeps public symbols. */
extern int tlsf_scan(struct MemHeaderExt *mhe, const char *where);

static void bootui_heap_check(const char *where)
{
    struct MemHeader *mh;

    if (bootui_heapscan < 0)
        bootui_heapscan = heapscan_wanted();
    if (!bootui_heapscan || !SysBase)
        return;

    ForeachNode(&SysBase->MemList, mh)
    {
        if (mh->mh_Attributes & MEMF_MANAGED)
            tlsf_scan((struct MemHeaderExt *)mh, where);
    }
}

void bootui_set_stage(uint32_t stage)
{
    const char *status;
    uint32_t progress;

    /* Before anything else, and before any early return: the whole value of
     * a checkpoint is that it happens at every one of them. */
    {
        uint32_t ignored;

        bootui_heap_check(stage_status(stage, &ignored));
    }

    if (stage == BOOTUI_STAGE_REPAINT)
    {
        bootui_repaint();
        return;
    }

    if (stage == BOOTUI_STAGE_DESKTOP)
        bootui_wanderer_started = 1;

    if (stage == BOOTUI_STAGE_HANDOVER)
    {
        /*
         * Let go of the framebuffer, for good.
         *
         * A native display driver now owns the hardware, and the first thing
         * such a driver does is program its own mode -- on this machine that
         * moves the surface and changes it from 16bpp to 32bpp. Everything
         * drawn here after that lands at the old geometry, which is not
         * "stale pixels" but active corruption: the clock tick alone was
         * enough to paint the splash twice across a framebuffer of the new
         * width.
         *
         * There is nothing to follow it to. Asking where the new surface is
         * would mean asking the driver, and a driver that answers questions
         * about a splash is the arrangement this is built to avoid. Once the
         * display has an owner, the boot presentation's raw-surface era is
         * over.
         */
        if (bootui.active)
        {
            bootui.active = 0;
            bootui_event("display handed over to a native driver");
        }
        return;
    }

    if (stage == BOOTUI_STAGE_PRESENTED)
    {
        /*
         * AROS put something on the display. graphics.library says only that;
         * what it means for the splash is decided here, which is the whole
         * point of the boundary living above the drivers.
         *
         * The first one is the desktop handoff candidate: at boot it is
         * Wanderer's screen, and covering the gap between that screen
         * appearing and its icons being drawn is what the hold is for. Any
         * later one is something else asking for the display -- an alert, a
         * requester, an application that started before Wanderer finished --
         * and whatever it is has a better claim than a splash. So it ends the
         * hold rather than being held behind it, which is the rule that keeps
         * errors from being hidden.
         *
         * Task context: LoadView() runs on whoever opened the screen, so
         * releasing from here is legal in a way that releasing from the timer
         * is not.
         */
        if (!bootui.active)
            return;

        if (!bootui_holding_seen)
        {
            bootui_holding_seen = 1;
            bootui_hold();
            return;
        }

        /*
         * A later presentation is only "something else wants the display"
         * until Wanderer is on its way. Before that, anything reaching the
         * display is an alert or a requester, has a better claim than a
         * splash, and must not be covered by one. After it, the presentations
         * are the desktop being assembled -- a mode change, a second screen --
         * and ending the hold on them would put a half-drawn Workbench up,
         * which is the whole thing the hold exists to prevent.
         *
         * From there the hold ends where it always did: on the icons, or on
         * its own deadline.
         */
        if (!bootui_wanderer_started && bootui_holding())
            bootui_release_now("hold released: another presentation");
        return;
    }

    if (stage == BOOTUI_STAGE_ICONS)
    {
        /*
         * Arms; does not release.
         *
         * "The icons are in the list" is not "the desktop is finished" -- the
         * screen title bar is drawn after this, and releasing here copies a
         * screen that does not have it yet, which is exactly what happened:
         * a black bar that nothing ever redraws, because intuition renders it
         * once at screen creation and thereafter only on input or window
         * activation. Waiting for the drawing to settle catches it; a signal
         * fired mid-way does not.
         */
        if (bootui_hold_active && !bootui_hold_armed)
        {
            bootui_hold_armed = 1;
            bootui_event("hold armed: icons");
        }
        return;
    }

    if (!bootui.active)
        return;
    status = stage_status(stage, &progress);
    if (!status || progress < bootui.progress)
        return;
    bootui.progress = progress;
    bootui.status = status;
    bootui_platform_log("[BootUI] ");
    bootui_platform_log(status);
    bootui_platform_log("\n");
    draw_progress(progress, status);
    draw_clock();
    if (stage == BOOTUI_STAGE_DESKTOP && bootui_direct_scanout)
    {
        bootui_event("display takeover: direct scanout");
        bootui.active = 0;
    }
}

void bootui_clock_start(uint32_t now_us)
{
    bootui.clock_start_us = now_us;
    bootui.elapsed_seconds = 0;
    bootui.clock_started = 1;
}

void bootui_clock_tick(uint32_t now_us)
{
    uint32_t elapsed;

    if (!bootui.active || !bootui.clock_started)
        return;
    bootui_last_tick_us = now_us;

    if (bootui_hold_active)
    {
        /* Checked every tick, not once a second: a burst of drawing between
         * seconds would otherwise go unseen and the screen be called settled
         * while it is still being built. */
        if (bootui_hold_drawing || !bootui_hold_quiet_valid)
        {
            bootui_hold_drawing = 0;
            bootui_hold_quiet_valid = 1;
            bootui_hold_quiet_since_us = now_us;
        }
        else if (bootui_hold_armed &&
                 now_us - bootui_hold_quiet_since_us >= BOOTUI_HOLD_QUIET_US)
        {
            bootui_event("hold released: settled");
            bootui_release();
            return;
        }
    }

    elapsed = (now_us - bootui.clock_start_us) / 1000000UL;
    if (elapsed == bootui.elapsed_seconds)
        return;
    bootui.elapsed_seconds = elapsed;

    if (bootui_hold_active)
    {
        if (elapsed - bootui_hold_started_at >= BOOTUI_HOLD_CAP_SECONDS)
        {
            bootui_event("hold released: cap");
            bootui_release();
            return;
        }
    }

    draw_clock();
}

void bootui_add_resource(void)
{
    bootui_resource.node.ln_Name = BOOTUI_RESOURCE_NAME;
    bootui_resource.node.ln_Pri = 0;
    bootui_resource.node.ln_Type = NT_RESOURCE;
    bootui_resource.set_stage = bootui_set_stage;
    AddResource(&bootui_resource.node);
}

/*
 * Decides to let go; does not let go.
 *
 * Safe from an interrupt: two flag writes and a raw serial write. In
 * particular it does NOT call bootui_takeover() -- that would stop the
 * splash being drawn while it is still the only thing on the screen, because
 * the repaint that replaces it cannot happen until a task calls
 * bootui_take_release(). Doing both here left the display showing a
 * frozen splash with a dead clock for the whole gap between the two, which is
 * exactly what a hold is supposed to prevent.
 */
static void bootui_release(void)
{
    if (!bootui_hold_active)
        return;

    bootui_hold_active = 0;
    bootui_release_pending = 1;
}

/*
 * End the hold and put the desktop up, from the caller's task.
 *
 * Must not be called from an interrupt: the hook takes the framebuffer
 * semaphore and walks OOP objects. The two callers are the icons signal, which
 * arrives on Wanderer's task, and the display driver's Show(), which runs on
 * whichever task opened a screen.
 */
void bootui_release_now(const char *why)
{
    void (*hook)(void) = bootui_release_hook;

    if (!bootui_hold_active)
        return;

    bootui_event(why);
    bootui_hold_active = 0;
    bootui_release_pending = 0;
    bootui_takeover();
    if (hook)
        hook();
}

/*
 * Copy the screen across again, after the release.
 *
 * The release copies the bitmap as it stands, and "as it stands" turned out to
 * include a screen title bar caught one character into its text -- a lone "W"
 * on black, still there in a screenshot taken five seconds later. Whatever
 * finishes that bar does not produce a refresh that reaches us, so a second
 * copy a beat later is what puts it on screen.
 *
 * Task context only, same as the hook itself.
 */
void bootui_repaint(void)
{
    if (bootui_release_hook)
        bootui_release_hook();
}

void bootui_set_release_hook(void (*hook)(void))
{
    bootui_release_hook = hook;
}

void bootui_hold(void)
{
    if (!bootui.active || bootui_hold_active)
        return;

    /*
     * Not this one. A screen opening before Wanderer was started is something
     * the user needs to see now, so the splash gets out of its way instead of
     * holding the display in front of it.
     */
    if (!bootui_wanderer_started)
    {
        bootui_takeover();
        return;
    }

    bootui_hold_started_at = bootui.elapsed_seconds;
    bootui_hold_quiet_valid = 0;
    bootui_hold_drawing = 0;
    bootui_hold_armed = 0;
    bootui_hold_active = 1;
    bootui_event("holding the display");
}

int bootui_holding(void)
{
    if (bootui_hold_active)
        bootui_hold_drawing = 1;
    return bootui_hold_active;
}

/*
 * Consume-once: TRUE means the hold has just ended and nothing of the desktop
 * has reached the framebuffer yet, so the caller owes one full-screen repaint.
 * Asked only from the driver's refresh path, which is task context.
 */
int bootui_take_release(void)
{
    int pending = bootui_release_pending;

    if (pending)
    {
        /*
         * Now, and not a moment earlier. The caller repaints the screen
         * immediately after this returns, so the splash stops being drawn and
         * gets covered in the same breath -- no interval where it is logically
         * gone and visually still there.
         */
        bootui_release_pending = 0;
        bootui_takeover();
    }
    return pending;
}

void bootui_takeover(void)
{
    if (bootui.active)
        bootui_event("display takeover");
    bootui.active = 0;
}
