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
    int active;
};

static struct BootUIState bootui;
static struct BootUIResource bootui_resource;

static void put_pixel(uint32_t x, uint32_t y, uint16_t color);

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

static int draw_boot_image(void)
{
    uint32_t run;
    uint16_t *pixel;

    if (bootui.width != BOOT_IMAGE_WIDTH || bootui.height != BOOT_IMAGE_HEIGHT)
        return 0;
    if (bootui.pitch != BOOT_IMAGE_WIDTH * 2)
        return 0;
    pixel = (uint16_t *)bootui.framebuffer;
    for (run = 0; run < sizeof(boot_image_runs) / sizeof(boot_image_runs[0]); run++)
    {
        uint32_t count;
        uint16_t colour = boot_image_runs[run].colour;

        colour = (colour << 8) | (colour >> 8);

        for (count = 0; count < boot_image_runs[run].count; count++)
            *pixel++ = colour;
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
    uint32_t bar_width = bootui.width / 2;
    uint32_t bar_height = 6;
    uint32_t bar_x = (bootui.width - bar_width) / 2;
    uint32_t bar_y = bootui.height - 26;
    uint16_t background = RGB565(0, 0, 0);
    uint16_t track = RGB565(29, 31, 38);
    uint16_t accent = RGB565(116, 83, 234);

    fill_rect(0, bootui.height - 90, bootui.width, 90, background);
    draw_text(status, bar_y - 28, 1, RGB565(190, 193, 203));
    fill_rect(bar_x, bar_y, bar_width, bar_height, track);
    fill_rect(bar_x, bar_y, bar_width * progress / 100, bar_height, accent);
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
    bootui.progress = 0;
    bootui.active = 1;

    if (!draw_boot_image())
        fill_rect(0, 0, bootui.width, bootui.height, background);
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
