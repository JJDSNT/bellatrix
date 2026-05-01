// src/host/posix/pal_posix.c
//
// PAL implementation for the harness (Linux host, x86_64).

#define _GNU_SOURCE
#define _XOPEN_SOURCE 600

#include "host/pal.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define HARNESS_FB_W 640u
#define HARNESS_FB_H 512u

static uint16_t s_pixels[HARNESS_FB_W * HARNESS_FB_H];

typedef enum HarnessSerialMode {
    HARNESS_SERIAL_LINE = 0,
    HARNESS_SERIAL_RAW,
    HARNESS_SERIAL_ANSI,
    HARNESS_SERIAL_PTY,
} HarnessSerialMode;

static HarnessSerialMode s_serial_mode = HARNESS_SERIAL_LINE;
static int s_serial_raw_line_start = 1;
static int s_serial_pty_fd = -1;
static char s_serial_pty_name[128];
static int s_serial_translate_ff_clear = 1;

uint16_t *framebuffer = NULL;
uint32_t pitch = 0;
uint32_t fb_width = 0;
uint32_t fb_height = 0;

static int pal_posix_configure_pty_slave_raw(const char *slave_name)
{
    struct termios tio;
    int slave_fd;

    if (!slave_name || slave_name[0] == '\0')
        return -1;

    slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0)
        return -1;

    if (tcgetattr(slave_fd, &tio) != 0) {
        close(slave_fd);
        return -1;
    }

    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(slave_fd, TCSANOW, &tio) != 0) {
        close(slave_fd);
        return -1;
    }

    close(slave_fd);
    return 0;
}

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

void PAL_IPL_Set(uint8_t ipl_level) { (void)ipl_level; }
void PAL_IPL_Clear(void) {}

void PAL_ChipsetTimer_Init(uint32_t hz, void (*cb)(void))
{
    (void)hz;
    (void)cb;
}

void PAL_ChipsetTimer_Start(void) {}
void PAL_ChipsetTimer_Stop(void) {}

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

void PAL_Runtime_Init(void) {}
void PAL_Runtime_Shutdown(void) {}
void PAL_Runtime_Poll(void) {}
void PAL_Runtime_ReportCpuProgress(uint32_t cycles) { (void)cycles; }
uint32_t PAL_Runtime_GetPendingIPL(void) { return 0; }
void PAL_Runtime_WakeupChipset(void) {}
void PAL_Runtime_MmioBarrier(void) {}

void PAL_HarnessSerial_ConfigureFromEnv(void)
{
    const char *mode = getenv("HARNESS_SERIAL_MODE");
    const char *ff_clear = getenv("HARNESS_SERIAL_FF_CLEAR");

    if (ff_clear && ff_clear[0] != '\0' && strcmp(ff_clear, "0") == 0)
        s_serial_translate_ff_clear = 0;
    else
        s_serial_translate_ff_clear = 1;

    if (!mode || mode[0] == '\0' || strcmp(mode, "line") == 0) {
        s_serial_mode = HARNESS_SERIAL_LINE;
        return;
    }

    if (strcmp(mode, "raw") == 0) {
        s_serial_mode = HARNESS_SERIAL_RAW;
        return;
    }

    if (strcmp(mode, "ansi") == 0) {
        s_serial_mode = HARNESS_SERIAL_ANSI;
        return;
    }

    if (strcmp(mode, "pty") == 0) {
        int fd = posix_openpt(O_RDWR | O_NOCTTY);

        if (fd < 0) {
            fprintf(stderr,
                    "[HARNESS] Failed to create PTY, using line mode\n");
            s_serial_mode = HARNESS_SERIAL_LINE;
            return;
        }

        if (grantpt(fd) != 0 || unlockpt(fd) != 0) {
            fprintf(stderr,
                    "[HARNESS] Failed to initialize PTY, using line mode\n");
            close(fd);
            s_serial_mode = HARNESS_SERIAL_LINE;
            return;
        }

        if (ptsname_r(fd, s_serial_pty_name, sizeof(s_serial_pty_name)) != 0) {
            fprintf(stderr,
                    "[HARNESS] Failed to resolve PTY name, using line mode\n");
            close(fd);
            s_serial_mode = HARNESS_SERIAL_LINE;
            return;
        }

        if (pal_posix_configure_pty_slave_raw(s_serial_pty_name) != 0) {
            fprintf(stderr,
                    "[HARNESS] Failed to set PTY slave raw mode, using line mode\n");
            close(fd);
            s_serial_mode = HARNESS_SERIAL_LINE;
            return;
        }

        s_serial_pty_fd = fd;
        (void)fcntl(s_serial_pty_fd, F_SETFL,
                    fcntl(s_serial_pty_fd, F_GETFL, 0) | O_NONBLOCK);
        s_serial_mode = HARNESS_SERIAL_PTY;
        fprintf(stderr,
                "[HARNESS] Serial PTY ready: %s\n",
                s_serial_pty_name);
        return;
    }

    fprintf(stderr,
            "[HARNESS] Unknown HARNESS_SERIAL_MODE=%s, using line\n",
            mode);
    s_serial_mode = HARNESS_SERIAL_LINE;
}

const char *PAL_HarnessSerial_ModeName(void)
{
    switch (s_serial_mode) {
    case HARNESS_SERIAL_RAW:
        return "raw";
    case HARNESS_SERIAL_ANSI:
        return "ansi";
    case HARNESS_SERIAL_PTY:
        return "pty";
    case HARNESS_SERIAL_LINE:
    default:
        return "line";
    }
}

void PAL_HarnessSerial_WriteByte(uint8_t byte)
{
    switch (s_serial_mode) {
    case HARNESS_SERIAL_ANSI:
        fputc((int)byte, stderr);
        fflush(stderr);
        return;

    case HARNESS_SERIAL_PTY:
        if (s_serial_pty_fd >= 0) {
            if (byte == 0x0Cu && s_serial_translate_ff_clear) {
                static const char clear_seq[] = "\x1b[2J\x1b[H";
                ssize_t ignored = write(s_serial_pty_fd,
                                        clear_seq,
                                        sizeof(clear_seq) - 1u);
                (void)ignored;
            } else {
                ssize_t ignored = write(s_serial_pty_fd, &byte, 1);
                (void)ignored;
            }
        }
        return;

    case HARNESS_SERIAL_RAW:
        if (s_serial_raw_line_start) {
            fputs("[SERIAL-RAW] ", stderr);
            s_serial_raw_line_start = 0;
        }

        if (byte == '\n') {
            fputc('\n', stderr);
            s_serial_raw_line_start = 1;
        } else if (byte == '\r') {
            fputs("\\r", stderr);
        } else if (byte == '\t') {
            fputs("\\t", stderr);
        } else if (byte == 0x1Bu) {
            fputs("\\e", stderr);
        } else if (byte < 32u || byte == 127u) {
            fprintf(stderr, "\\x%02x", (unsigned)byte);
        } else {
            fputc((int)byte, stderr);
        }
        fflush(stderr);
        return;

    case HARNESS_SERIAL_LINE:
    default:
        return;
    }
}

int PAL_HarnessSerial_ReadByte(uint8_t *byte)
{
    ssize_t nread;

    if (!byte || s_serial_mode != HARNESS_SERIAL_PTY || s_serial_pty_fd < 0)
        return 0;

    nread = read(s_serial_pty_fd, byte, 1);
    if (nread == 1)
        return 1;

    if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;

    return 0;
}

void PAL_Core_LaunchChipset(void (*entry)(void)) { (void)entry; }
void PAL_Core_SetMulticoreEnabled(int enabled) { (void)enabled; }
int PAL_Core_IsMulticoreEnabled(void) { return 0; }
void PAL_Core_Sync(void) {}

#ifdef BELLATRIX_SDL2
#include <SDL2/SDL.h>

static SDL_Window *s_window = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture *s_texture = NULL;
static int s_mouse_right_down = 0;
static int s_any_key_down = 0;
static PAL_KeyEvent s_key_events[64];
static uint8_t s_key_head = 0;
static uint8_t s_key_tail = 0;
static uint8_t s_key_count = 0;

static uint32_t pal_map_sdl_host_key(SDL_Scancode scancode)
{
    switch (scancode) {
    case SDL_SCANCODE_1: return PAL_HOST_KEY_1;
    case SDL_SCANCODE_2: return PAL_HOST_KEY_2;
    case SDL_SCANCODE_3: return PAL_HOST_KEY_3;
    case SDL_SCANCODE_4: return PAL_HOST_KEY_4;
    case SDL_SCANCODE_5: return PAL_HOST_KEY_5;
    case SDL_SCANCODE_6: return PAL_HOST_KEY_6;
    case SDL_SCANCODE_7: return PAL_HOST_KEY_7;
    case SDL_SCANCODE_8: return PAL_HOST_KEY_8;
    case SDL_SCANCODE_9: return PAL_HOST_KEY_9;
    case SDL_SCANCODE_0: return PAL_HOST_KEY_0;
    case SDL_SCANCODE_SPACE: return PAL_HOST_KEY_SPACE;
    case SDL_SCANCODE_RETURN: return PAL_HOST_KEY_RETURN;
    case SDL_SCANCODE_KP_ENTER: return PAL_HOST_KEY_KP_ENTER;
    case SDL_SCANCODE_ESCAPE: return PAL_HOST_KEY_ESCAPE;
    case SDL_SCANCODE_UP: return PAL_HOST_KEY_UP;
    case SDL_SCANCODE_DOWN: return PAL_HOST_KEY_DOWN;
    case SDL_SCANCODE_LEFT: return PAL_HOST_KEY_LEFT;
    case SDL_SCANCODE_RIGHT: return PAL_HOST_KEY_RIGHT;
    case SDL_SCANCODE_KP_0: return PAL_HOST_KEY_KP_0;
    case SDL_SCANCODE_KP_1: return PAL_HOST_KEY_KP_1;
    case SDL_SCANCODE_KP_2: return PAL_HOST_KEY_KP_2;
    case SDL_SCANCODE_KP_3: return PAL_HOST_KEY_KP_3;
    case SDL_SCANCODE_KP_4: return PAL_HOST_KEY_KP_4;
    case SDL_SCANCODE_KP_5: return PAL_HOST_KEY_KP_5;
    case SDL_SCANCODE_KP_6: return PAL_HOST_KEY_KP_6;
    case SDL_SCANCODE_KP_7: return PAL_HOST_KEY_KP_7;
    case SDL_SCANCODE_KP_8: return PAL_HOST_KEY_KP_8;
    case SDL_SCANCODE_KP_9: return PAL_HOST_KEY_KP_9;
    default:
        return PAL_HOST_KEY_NONE;
    }
}

static void pal_queue_key_event(uint32_t keycode, uint32_t scancode, uint8_t pressed)
{
    if (s_key_count >= (uint8_t)(sizeof(s_key_events) / sizeof(s_key_events[0])))
        return;

    s_key_events[s_key_tail].keycode = keycode;
    s_key_events[s_key_tail].scancode = scancode;
    s_key_events[s_key_tail].host_key = pal_map_sdl_host_key((SDL_Scancode)scancode);
    s_key_events[s_key_tail].pressed = pressed;
    s_key_tail = (uint8_t)((s_key_tail + 1u) % (sizeof(s_key_events) / sizeof(s_key_events[0])));
    s_key_count++;
}

int PAL_Video_Init(uint32_t w, uint32_t h, uint32_t bpp)
{
    (void)bpp;

    if (w == 0)
        w = HARNESS_FB_W;
    if (h == 0)
        h = HARNESS_FB_H;

    if (w > HARNESS_FB_W)
        w = HARNESS_FB_W;
    if (h > HARNESS_FB_H)
        h = HARNESS_FB_H;

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        fprintf(stderr, "[PAL] SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    s_window = SDL_CreateWindow("Bellatrix",
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                (int)w,
                                (int)h,
                                SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!s_window)
    {
        fprintf(stderr, "[PAL] SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
                                    SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer)
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_SOFTWARE);

    if (!s_renderer)
    {
        fprintf(stderr, "[PAL] SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }

    s_texture = SDL_CreateTexture(s_renderer,
                                  SDL_PIXELFORMAT_RGB565,
                                  SDL_TEXTUREACCESS_STREAMING,
                                  (int)w,
                                  (int)h);
    if (!s_texture)
    {
        fprintf(stderr, "[PAL] SDL_CreateTexture: %s\n", SDL_GetError());
        return -1;
    }

    framebuffer = s_pixels;
    pitch = w * sizeof(uint16_t);
    fb_width = w;
    fb_height = h;

    for (uint32_t i = 0; i < fb_width * fb_height; ++i)
        framebuffer[i] = 0xF800; /* vermelho RGB565 */

    fprintf(stderr,
            "[PAL] video init framebuffer=%p pitch=%u size=%ux%u first=%04x\n",
            (void *)framebuffer,
            pitch,
            fb_width,
            fb_height,
            framebuffer[0]);

    PAL_Video_Flip();

    return 0;
}

uint32_t *PAL_Video_GetBuffer(void)
{
    return (uint32_t *)s_pixels;
}

void PAL_Video_Flip(void)
{
    static uint32_t flip_count = 0;

    if (!s_texture || !s_renderer || !framebuffer)
        return;

    flip_count++;

    SDL_UpdateTexture(s_texture, NULL, framebuffer, (int)pitch);
    SDL_RenderClear(s_renderer);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

void PAL_Video_SetPalette(uint8_t idx, uint32_t rgb)
{
    (void)idx;
    (void)rgb;
}

int pal_sdl_poll_events(void)
{
    SDL_Event e;

    while (SDL_PollEvent(&e))
    {
        if (e.type == SDL_QUIT)
            return 0;

        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
            return 0;

        if (e.type == SDL_KEYDOWN && e.key.repeat == 0)
        {
            s_any_key_down = 1;
            pal_queue_key_event((uint32_t)e.key.keysym.sym,
                                (uint32_t)e.key.keysym.scancode,
                                1u);
        }

        if (e.type == SDL_KEYUP)
        {
            s_any_key_down = 0;
            pal_queue_key_event((uint32_t)e.key.keysym.sym,
                                (uint32_t)e.key.keysym.scancode,
                                0u);
        }

        if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT)
            s_mouse_right_down = 1;

        if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT)
            s_mouse_right_down = 0;
    }

    return 1;
}

int pal_sdl_mouse_right_down(void)
{
    return s_mouse_right_down;
}

int pal_sdl_any_key_down(void)
{
    return s_any_key_down;
}

int pal_sdl_pop_key_event(PAL_KeyEvent *event)
{
    if (!event || s_key_count == 0)
        return 0;

    *event = s_key_events[s_key_head];
    s_key_head = (uint8_t)((s_key_head + 1u) % (sizeof(s_key_events) / sizeof(s_key_events[0])));
    s_key_count--;
    return 1;
}

void pal_sdl_set_title(const char *title)
{
    if (s_window)
        SDL_SetWindowTitle(s_window, title);
}

#else

int PAL_Video_Init(uint32_t w, uint32_t h, uint32_t bpp)
{
    (void)bpp;

    if (w == 0)
        w = HARNESS_FB_W;
    if (h == 0)
        h = HARNESS_FB_H;

    if (w > HARNESS_FB_W)
        w = HARNESS_FB_W;
    if (h > HARNESS_FB_H)
        h = HARNESS_FB_H;

    framebuffer = s_pixels;
    pitch = w * sizeof(uint16_t);
    fb_width = w;
    fb_height = h;

    for (uint32_t i = 0; i < fb_width * fb_height; ++i)
        framebuffer[i] = 0xF800;

    fprintf(stderr,
            "[PAL] headless video init framebuffer=%p pitch=%u size=%ux%u first=%04x\n",
            (void *)framebuffer,
            pitch,
            fb_width,
            fb_height,
            framebuffer[0]);

    return 0;
}

uint32_t *PAL_Video_GetBuffer(void)
{
    return (uint32_t *)s_pixels;
}

void PAL_Video_Flip(void) {}

void PAL_Video_SetPalette(uint8_t idx, uint32_t rgb)
{
    (void)idx;
    (void)rgb;
}

int pal_sdl_poll_events(void) { return 1; }
int pal_sdl_mouse_right_down(void) { return 0; }
int pal_sdl_any_key_down(void) { return 0; }
int pal_sdl_pop_key_event(PAL_KeyEvent *event)
{
    (void)event;
    return 0;
}

void pal_sdl_set_title(const char *title)
{
    (void)title;
}

#endif
