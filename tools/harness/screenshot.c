// tools/harness/screenshot.c
// Headless framebuffer screenshots — see screenshot.h.

#include "screenshot.h"
#include "machine/expansions/rtg/rtg.h"

#include "machine/machine.h"
#include "rigel/rigel.h"
#include "rigel/rigel_denise_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void harness_maybe_screenshot(long frame_count)
{
    static const char *frames_env = NULL;
    static int checked = 0;
    if (!checked) {
        frames_env = getenv("HARNESS_SCREENSHOT_FRAMES");
        checked = 1;
    }
    if (!frames_env)
        return;

    /* is frame_count listed? */
    const char *p = frames_env;
    int match = 0;
    while (*p) {
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        if (v == frame_count) { match = 1; break; }
        p = (*end == ',') ? end + 1 : end;
        if (end == p && *p) break;
    }
    if (!match)
        return;

    struct RigelContext *ctx = bellatrix_machine_rigel_ctx();
    rigel_frame_t frame;
    BellatrixRtgFrame rtg_frame;
    if (bellatrix_rtg_get_frame(&rtg_frame)) {
        /* RTG owns the output (ENABLE set by the guest driver) */
        frame.pixels = (void *)rtg_frame.pixels;
        frame.width  = rtg_frame.width;
        frame.height = rtg_frame.height;
        frame.pitch  = rtg_frame.pitch;
        frame.format = RIGEL_PIXEL_RGBA8888;
    } else if (!ctx || !rigel_get_frame(ctx, &frame) || !frame.pixels) {
        fprintf(stderr, "[SCREENSHOT] frame %ld: no frame available\n", frame_count);
        return;
    }

    const char *dir = getenv("HARNESS_SCREENSHOT_DIR");
    char path[512];
    snprintf(path, sizeof(path), "%s/shot_%ld.ppm", dir ? dir : ".", frame_count);

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[SCREENSHOT] cannot open %s\n", path);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", frame.width, frame.height);
    for (uint32_t y = 0; y < frame.height; y++) {
        const uint8_t *row = (const uint8_t *)frame.pixels + (size_t)y * frame.pitch;
        for (uint32_t x = 0; x < frame.width; x++) {
            /* Pixel memory byte order is [R,G,B,A]; PPM wants R,G,B. */
            fwrite(row + x * 4u, 1, 3, f);
        }
    }
    fclose(f);

    /* Optional chip RAM dump at the same frame */
    {
        const char *cd = getenv("HARNESS_CHIPDUMP");
        BellatrixMachine *m = bellatrix_machine_get();
        if (cd && m && m->memory.chip_ram) {
            unsigned long a = 0, l = 0;
            if (sscanf(cd, "%lx:%lx", &a, &l) == 2 && l > 0 &&
                a + l <= m->memory.chip_ram_size) {
                char cpath[512];
                snprintf(cpath, sizeof(cpath), "%s/chip_%ld_%05lx.bin",
                         dir ? dir : ".", frame_count, a);
                FILE *cf = fopen(cpath, "wb");
                if (cf) {
                    fwrite(m->memory.chip_ram + a, 1, l, cf);
                    fclose(cf);
                    printf("[SCREENSHOT] chip dump %05lx+%lx → %s\n", a, l, cpath);
                }
            }
        }
    }

    {
        const uint8_t *p0 = (const uint8_t *)frame.pixels;
        const uint8_t *pc = p0 + (frame.height / 2u) * frame.pitch
                               + (frame.width / 2u) * 4u;
        printf("[SCREENSHOT] frame %ld → %s (%ux%u fmt=%d) "
               "px(0,0)=%02x %02x %02x %02x px(c)=%02x %02x %02x %02x\n",
               frame_count, path, frame.width, frame.height, (int)frame.format,
               p0[0], p0[1], p0[2], p0[3], pc[0], pc[1], pc[2], pc[3]);
    }
}
