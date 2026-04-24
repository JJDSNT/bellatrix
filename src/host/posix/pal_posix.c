// src/host/posix/pal_posix.c
//
// PAL implementation for the harness (Linux host, x86_64).
// PAL_Video_* uses SDL2 when BELLATRIX_SDL2 is defined; otherwise headless stubs.

#include "host/pal.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Framebuffer globals — referenced directly by denise.c
 * In SDL2 mode these point at a static host-side pixel buffer.
 * PAL_Video_Flip() uploads it to the SDL texture and presents.
 * ------------------------------------------------------------------------- */

#define HARNESS_FB_W  640u
#define HARNESS_FB_H  512u

static uint16_t s_pixels[HARNESS_FB_W * HARNESS_FB_H];

uint16_t *framebuffer = NULL;
uint32_t  pitch       = 0;
uint32_t  fb_width    = 0;
uint32_t  fb_height   = 0;

/* ---------------------------------------------------------------------------
 * Debug / Serial
 * ------------------------------------------------------------------------- */

void PAL_Debug_Init(uint32_t baud) { (void)baud; }

void PAL_Debug_PutC(char c)
{
    putchar((unsigned char)c);
    fflush(stdout);
}

void PAL_Debug_Print(const char *str)
{
    fputs(str, stdout);
    fflush(stdout);
}

void PAL_Debug_PrintHex(uint32_t val)
{
    printf("0x%08x", (unsigned)val);
    fflush(stdout);
}

/* ---------------------------------------------------------------------------
 * IPL — handled by musashi_backend via CpuBackend.set_ipl
 * ------------------------------------------------------------------------- */

void PAL_IPL_Set(uint8_t ipl_level) { (void)ipl_level; }
void PAL_IPL_Clear(void) {}

/* ---------------------------------------------------------------------------
 * Chipset timer (stub)
 * ------------------------------------------------------------------------- */

void PAL_ChipsetTimer_Init(uint32_t hz, void (*cb)(void)) { (void)hz; (void)cb; }
void PAL_ChipsetTimer_Start(void) {}
void PAL_ChipsetTimer_Stop(void) {}

/* ---------------------------------------------------------------------------
 * Time
 * ------------------------------------------------------------------------- */

uint64_t PAL_Time_ReadCounter(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t PAL_Time_GetFrequency(void)
{
    return 1000000000ULL;
}

/* ---------------------------------------------------------------------------
 * Runtime lifecycle (stubs — harness drives the loop)
 * ------------------------------------------------------------------------- */

void PAL_Runtime_Init(void) {}
void PAL_Runtime_Shutdown(void) {}
void PAL_Runtime_Poll(void) {}
void PAL_Runtime_ReportCpuProgress(uint32_t cycles) { (void)cycles; }
uint32_t PAL_Runtime_GetPendingIPL(void) { return 0; }
void PAL_Runtime_WakeupChipset(void) {}
void PAL_Runtime_MmioBarrier(void) {}

/* ---------------------------------------------------------------------------
 * Dedicated ARM core (stub)
 * ------------------------------------------------------------------------- */

void PAL_Core_LaunchChipset(void (*entry)(void)) { (void)entry; }
void PAL_Core_SetMulticoreEnabled(int enabled) { (void)enabled; }
int  PAL_Core_IsMulticoreEnabled(void) { return 0; }
void PAL_Core_Sync(void) {}

/* ---------------------------------------------------------------------------
 * Video — SDL2 or headless
 * ------------------------------------------------------------------------- */

#ifdef BELLATRIX_SDL2
#include <SDL2/SDL.h>

static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;

int PAL_Video_Init(uint32_t w, uint32_t h, uint32_t bpp)
{
    (void)bpp;

    if (w == 0) w = HARNESS_FB_W;
    if (h == 0) h = HARNESS_FB_H;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[PAL] SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    s_window = SDL_CreateWindow("Bellatrix",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                (int)w, (int)h,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!s_window) {
        fprintf(stderr, "[PAL] SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer)
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);
    if (!s_renderer) {
        fprintf(stderr, "[PAL] SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }

    s_texture = SDL_CreateTexture(s_renderer,
                                  SDL_PIXELFORMAT_RGB565,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  (int)w, (int)h);
    if (!s_texture) {
        fprintf(stderr, "[PAL] SDL_CreateTexture: %s\n", SDL_GetError());
        return -1;
    }

    /* Wire the globals that denise.c reads */
    memset(s_pixels, 0, sizeof(s_pixels));
    framebuffer = s_pixels;
    pitch       = w * sizeof(uint16_t);
    fb_width    = w;
    fb_height   = h;

    return 0;
}

uint32_t *PAL_Video_GetBuffer(void) { return (uint32_t *)s_pixels; }

void PAL_Video_Flip(void)
{
    if (!s_texture || !framebuffer) return;

    SDL_UpdateTexture(s_texture, NULL, framebuffer, (int)pitch);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

void PAL_Video_SetPalette(uint8_t idx, uint32_t rgb) { (void)idx; (void)rgb; }

/* Exposed to harness main loop for event polling */
int pal_sdl_poll_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            return 0;
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
            return 0;
    }
    return 1;
}

void pal_sdl_set_title(const char *title)
{
    if (s_window) SDL_SetWindowTitle(s_window, title);
}

#else /* BELLATRIX_SDL2 not defined — headless */

int PAL_Video_Init(uint32_t w, uint32_t h, uint32_t bpp)
{
    (void)bpp;
    if (w == 0) w = HARNESS_FB_W;
    if (h == 0) h = HARNESS_FB_H;

    /* Provide a real pixel buffer even in headless mode so Denise doesn't
     * skip the render.  Useful for debugging palette/bitplane state. */
    memset(s_pixels, 0, sizeof(s_pixels));
    framebuffer = s_pixels;
    pitch       = w * sizeof(uint16_t);
    fb_width    = w;
    fb_height   = h;
    return 0;
}

uint32_t *PAL_Video_GetBuffer(void) { return (uint32_t *)s_pixels; }
void     PAL_Video_Flip(void) {}
void     PAL_Video_SetPalette(uint8_t idx, uint32_t rgb) { (void)idx; (void)rgb; }

int  pal_sdl_poll_events(void) { return 1; }
void pal_sdl_set_title(const char *title) { (void)title; }

#endif /* BELLATRIX_SDL2 */
