/*
 * Minimal early-boot UI for the Emu68 linear RGB16_LE framebuffer.
 *
 * This deliberately has no Exec, graphics.library or font dependencies: it
 * is first called before SysBase exists.  All pixels are stored bytewise so
 * the little-endian framebuffer format is correct on the big-endian 68k.
 */

#include "boot.h"

#include <aros/bootui.h>
#include <exec/nodes.h>
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
    uint32_t progress;
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
    pixel = bootui.framebuffer + y * bootui.pitch + x * 2;
    pixel[0] = color & 0xff;
    pixel[1] = color >> 8;
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
    case EMU68_STAGE_ENTRY:
        *progress = 2;
        return "STARTING BELLATRIX...";
    case EMU68_STAGE_EXEC_READY:
        *progress = 6;
        return "STARTING EXEC...";
    case EMU68_STAGE_SINGLETASK:
        *progress = 9;
        return "INITIALIZING SYSTEM...";
    case EMU68_STAGE_KERNEL_READY:
        *progress = 12;
        return "STARTING KERNEL...";
    case EMU68_STAGE_COLDSTART:
        *progress = 15;
        return "LOADING SYSTEM...";
    case EMU68_STAGE_GRAPHICS_READY:
        *progress = 20;
        return "STARTING GRAPHICS...";
    case EMU68_STAGE_DOS_READY:
        *progress = 28;
        return "STARTING DOS...";
    case EMU68_STAGE_STARTUP:
        *progress = 50;
        return "STARTING SERVICES...";
    case EMU68_STAGE_DESKTOP:
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

void emu68_bootui_init(void)
{
    struct Emu68BootContext *ctx = &emu68_boot_context;
    uint16_t background = RGB565(7, 12, 24);

    bootui.active = 0;
    if (!(ctx->flags & EMU68_BOOT_FRAMEBUFFER) ||
        !ctx->framebuffer || !ctx->framebuffer_pitch ||
        !ctx->framebuffer_width || !ctx->framebuffer_height ||
        ctx->framebuffer_pitch < ctx->framebuffer_width * 2)
        return;

    bootui.framebuffer = ctx->framebuffer;
    bootui.pitch = ctx->framebuffer_pitch;
    bootui.width = ctx->framebuffer_width;
    bootui.height = ctx->framebuffer_height;

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
        emu68_console_puts("[AROS/Emu68] BootUI: framebuffer too small for "
                           "the boot image\n");
}

void emu68_bootui_set_stage(uint32_t stage)
{
    const char *status;
    uint32_t progress;

    if (!bootui.active)
        return;
    status = stage_status(stage, &progress);
    if (!status || progress < bootui.progress)
        return;
    bootui.progress = progress;
    emu68_console_puts("[AROS/Emu68] BootUI: ");
    emu68_console_puts(status);
    emu68_console_puts("\n");
    draw_progress(progress, status);
    draw_clock();
}

void emu68_bootui_clock_start(uint32_t now_us)
{
    bootui.clock_start_us = now_us;
    bootui.elapsed_seconds = 0;
    bootui.clock_started = 1;
}

void emu68_bootui_clock_tick(uint32_t now_us)
{
    uint32_t elapsed;

    if (!bootui.active || !bootui.clock_started)
        return;
    elapsed = (now_us - bootui.clock_start_us) / 1000000UL;
    if (elapsed == bootui.elapsed_seconds)
        return;
    bootui.elapsed_seconds = elapsed;
    draw_clock();
}

void emu68_bootui_add_resource(void)
{
    bootui_resource.node.ln_Name = BOOTUI_RESOURCE_NAME;
    bootui_resource.node.ln_Pri = 0;
    bootui_resource.node.ln_Type = NT_RESOURCE;
    bootui_resource.set_stage = emu68_bootui_set_stage;
    AddResource(&bootui_resource.node);
}

void emu68_bootui_takeover(void)
{
    if (bootui.active)
        emu68_console_puts("[AROS/Emu68] BootUI display takeover\n");
    bootui.active = 0;
}
