// tools/harness/main.c
//
// Bellatrix + Musashi harness.
//
// Interactive mode (default): opens an SDL2 window and runs until the user
// closes it.  Shows the Amiga display rendered by Denise in real time.
//
// Headless mode (--frames / --cycles): runs without display until the budget
// is reached then exits.  Useful for CI and chipset validation.
//
// Usage:
//   harness <rom.bin> [--adf disk.adf] [--headless] [--cycles N] [--frames N]

#include "musashi_backend.h"

#include "core/machine.h"
#include "chipset/paula/paula.h"
#include "debug/debug.h"
#include "host/pal.h"
#include "memory/memory.h"
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>

typedef struct HarnessSerialInject {
    uint8_t byte;
    long frame;
    int enabled;
    int injected;
    int consumed;
} HarnessSerialInject;

#define HARNESS_MAX_SERIAL_SCRIPT 16
#define HARNESS_MAX_SERIAL_HOLD 128

typedef struct HarnessMouseHold {
    long start_frame;
    long count;
    unsigned port;
    int enabled;
    int active;
} HarnessMouseHold;

typedef struct HarnessMouseSerialTrigger {
    uint8_t byte;
    long delay_frames;
    unsigned port;
    int enabled;
    int waiting_release;
    HarnessSerialInject pending;
} HarnessMouseSerialTrigger;

static void harness_fatal_signal(int sig)
{
    void *frames[64];
    int count = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));

    fprintf(stderr, "\n[HARNESS] fatal signal %d\n", sig);
    backtrace_symbols_fd(frames, count, STDERR_FILENO);
    _exit(128 + sig);
}

static void harness_install_signal_handlers(void)
{
    signal(SIGSEGV, harness_fatal_signal);
    signal(SIGABRT, harness_fatal_signal);
    signal(SIGBUS, harness_fatal_signal);
    signal(SIGILL, harness_fatal_signal);
}

static void harness_wait_for_serial_attach(void)
{
    char line[8];

    if (strcmp(PAL_HarnessSerial_ModeName(), "pty") != 0)
        return;

    if (getenv("HARNESS_SERIAL_NOWAIT"))
        return;

    fprintf(stderr,
            "[HARNESS] Waiting for serial terminal attach. "
            "Press Enter here to continue boot.\n");
    fflush(stderr);

    if (!fgets(line, sizeof(line), stdin))
        fprintf(stderr, "[HARNESS] stdin closed; continuing boot.\n");
}

static int harness_parse_serial_byte(const char *value, uint8_t *out)
{
    char *end = NULL;
    unsigned long v;

    if (!value || !*value)
        return 0;

    if (value[0] == '\\' && value[1] != '\0' && value[2] == '\0') {
        switch (value[1]) {
        case 'n':
            *out = '\n';
            return 1;
        case 'r':
            *out = '\r';
            return 1;
        case 't':
            *out = '\t';
            return 1;
        case '0':
            *out = '\0';
            return 1;
        default:
            *out = (uint8_t)value[1];
            return 1;
        }
    }

    if (value[1] == '\0') {
        *out = (uint8_t)value[0];
        return 1;
    }

    v = strtoul(value, &end, 0);
    if (!end || *end != '\0' || v > 0xFFu)
        return 0;

    *out = (uint8_t)v;
    return 1;
}

static void harness_load_serial_inject(const char *value_name,
                                       const char *frame_name,
                                       long default_frame,
                                       HarnessSerialInject *cfg)
{
    const char *value = getenv(value_name);
    const char *frame = getenv(frame_name);
    uint8_t byte = 0;

    memset(cfg, 0, sizeof(*cfg));

    if (!value)
        return;

    if (!harness_parse_serial_byte(value, &byte)) {
        fprintf(stderr,
                "[HARNESS] Ignoring %s=%s (expected single char or 0xNN)\n",
                value_name, value);
        return;
    }

    cfg->byte = byte;
    cfg->frame = default_frame;
    cfg->enabled = 1;

    if (frame && *frame) {
        char *end = NULL;
        long parsed = strtol(frame, &end, 10);
        if (end && *end == '\0' && parsed >= 0) {
            cfg->frame = parsed;
        } else {
            fprintf(stderr,
                    "[HARNESS] Ignoring %s=%s (expected non-negative frame)\n",
                    frame_name, frame);
        }
    }
}

static void harness_maybe_inject_serial(BellatrixMachine *m,
                                        long frame_count,
                                        HarnessSerialInject *cfg,
                                        const char *label)
{
    if (!cfg->enabled || cfg->injected || frame_count < cfg->frame)
        return;

    uart_receive_byte(&m->paula.uart, cfg->byte);
    cfg->injected = 1;

    if (cfg->byte >= 32 && cfg->byte <= 126) {
        fprintf(stderr,
                "[HARNESS] Injected serial %s at frame %ld: '%c' (0x%02x)\n",
                label, frame_count, (char)cfg->byte, (unsigned)cfg->byte);
    } else {
        fprintf(stderr,
                "[HARNESS] Injected serial %s at frame %ld: 0x%02x\n",
                label, frame_count, (unsigned)cfg->byte);
    }
}

static void harness_maybe_log_serial_consumed(BellatrixMachine *m,
                                              long frame_count,
                                              HarnessSerialInject *cfg,
                                              const char *label)
{
    if (!cfg->enabled || !cfg->injected || cfg->consumed)
        return;

    if (m->paula.uart.rx_buffer_full)
        return;

    cfg->consumed = 1;

    if (cfg->byte >= 32 && cfg->byte <= 126) {
        fprintf(stderr,
                "[HARNESS] Serial %s consumed by guest at frame %ld: '%c' (0x%02x)\n",
                label, frame_count, (char)cfg->byte, (unsigned)cfg->byte);
    } else {
        fprintf(stderr,
                "[HARNESS] Serial %s consumed by guest at frame %ld: 0x%02x\n",
                label, frame_count, (unsigned)cfg->byte);
    }
}

static int harness_load_serial_script(HarnessSerialInject *cfgs, size_t cap)
{
    const char *script = getenv("HARNESS_SERIAL_SCRIPT");
    char *copy = NULL;
    char *item = NULL;
    char *save = NULL;
    size_t count = 0;

    if (!script || !*script || cap == 0)
        return 0;

    copy = strdup(script);
    if (!copy) {
        fprintf(stderr, "[HARNESS] OOM parsing HARNESS_SERIAL_SCRIPT\n");
        return 0;
    }

    for (item = strtok_r(copy, ",", &save);
         item && count < cap;
         item = strtok_r(NULL, ",", &save)) {
        char *sep = strchr(item, ':');
        char *end = NULL;
        long frame = 0;
        uint8_t byte = 0;

        while (*item == ' ' || *item == '\t')
            item++;

        if (!sep) {
            fprintf(stderr,
                    "[HARNESS] Ignoring script item '%s' (expected frame:value)\n",
                    item);
            continue;
        }

        *sep = '\0';
        sep++;
        while (*sep == ' ' || *sep == '\t')
            sep++;

        frame = strtol(item, &end, 10);
        if (!end || *end != '\0' || frame < 0) {
            fprintf(stderr,
                    "[HARNESS] Ignoring script item '%s' (invalid frame)\n",
                    item);
            continue;
        }

        if (!harness_parse_serial_byte(sep, &byte)) {
            fprintf(stderr,
                    "[HARNESS] Ignoring script item '%s:%s' (invalid byte)\n",
                    item, sep);
            continue;
        }

        memset(&cfgs[count], 0, sizeof(cfgs[count]));
        cfgs[count].byte = byte;
        cfgs[count].frame = frame;
        cfgs[count].enabled = 1;
        count++;
    }

    free(copy);
    return (int)count;
}

static int harness_append_serial_hold(HarnessSerialInject *cfgs,
                                      size_t start_index,
                                      size_t cap)
{
    const char *value = getenv("HARNESS_SERIAL_HOLD");
    const char *frame_str = getenv("HARNESS_SERIAL_HOLD_FRAME");
    const char *count_str = getenv("HARNESS_SERIAL_HOLD_COUNT");
    const char *step_str = getenv("HARNESS_SERIAL_HOLD_STEP");
    char *end = NULL;
    uint8_t byte = 0;
    long frame = 0;
    long count = 0;
    long step = 1;
    size_t added = 0;

    if (!value || !*value)
        return 0;

    if (!harness_parse_serial_byte(value, &byte)) {
        fprintf(stderr,
                "[HARNESS] Ignoring HARNESS_SERIAL_HOLD=%s (expected single char or 0xNN)\n",
                value);
        return 0;
    }

    if (!frame_str || !*frame_str || !count_str || !*count_str) {
        fprintf(stderr,
                "[HARNESS] HARNESS_SERIAL_HOLD requires HARNESS_SERIAL_HOLD_FRAME and HARNESS_SERIAL_HOLD_COUNT\n");
        return 0;
    }

    frame = strtol(frame_str, &end, 10);
    if (!end || *end != '\0' || frame < 0) {
        fprintf(stderr,
                "[HARNESS] Ignoring HARNESS_SERIAL_HOLD_FRAME=%s (expected non-negative frame)\n",
                frame_str);
        return 0;
    }

    count = strtol(count_str, &end, 10);
    if (!end || *end != '\0' || count <= 0) {
        fprintf(stderr,
                "[HARNESS] Ignoring HARNESS_SERIAL_HOLD_COUNT=%s (expected positive count)\n",
                count_str);
        return 0;
    }

    if (step_str && *step_str) {
        step = strtol(step_str, &end, 10);
        if (!end || *end != '\0' || step <= 0) {
            fprintf(stderr,
                    "[HARNESS] Ignoring HARNESS_SERIAL_HOLD_STEP=%s (expected positive step)\n",
                    step_str);
            step = 1;
        }
    }

    for (long i = 0; i < count && start_index + added < cap; i++) {
        memset(&cfgs[start_index + added], 0, sizeof(cfgs[start_index + added]));
        cfgs[start_index + added].byte = byte;
        cfgs[start_index + added].frame = frame + (i * step);
        cfgs[start_index + added].enabled = 1;
        added++;
    }

    if (added < (size_t)count) {
        fprintf(stderr,
                "[HARNESS] Truncated serial hold sequence to %zu entries\n",
                added);
    }

    return (int)added;
}

static void harness_load_mouse_hold(HarnessMouseHold *cfg)
{
    const char *frame_str = getenv("HARNESS_MOUSE_RMB_FRAME");
    const char *count_str = getenv("HARNESS_MOUSE_RMB_COUNT");
    const char *port_str = getenv("HARNESS_MOUSE_RMB_PORT");
    char *end = NULL;

    memset(cfg, 0, sizeof(*cfg));

    if (!frame_str || !*frame_str || !count_str || !*count_str)
        return;

    cfg->start_frame = strtol(frame_str, &end, 10);
    if (!end || *end != '\0' || cfg->start_frame < 0) {
        fprintf(stderr,
                "[HARNESS] Ignoring HARNESS_MOUSE_RMB_FRAME=%s (expected non-negative frame)\n",
                frame_str);
        memset(cfg, 0, sizeof(*cfg));
        return;
    }

    cfg->count = strtol(count_str, &end, 10);
    if (!end || *end != '\0' || cfg->count <= 0) {
        fprintf(stderr,
                "[HARNESS] Ignoring HARNESS_MOUSE_RMB_COUNT=%s (expected positive count)\n",
                count_str);
        memset(cfg, 0, sizeof(*cfg));
        return;
    }

    cfg->port = 0u;
    if (port_str && *port_str) {
        unsigned long port = strtoul(port_str, &end, 10);
        if (!end || *end != '\0' || port > 1u) {
            fprintf(stderr,
                    "[HARNESS] Ignoring HARNESS_MOUSE_RMB_PORT=%s (expected 0 or 1)\n",
                    port_str);
        } else {
            cfg->port = (unsigned)port;
        }
    }

    cfg->enabled = 1;
}

static void harness_update_mouse_hold(BellatrixMachine *m,
                                      long frame_count,
                                      HarnessMouseHold *cfg)
{
    int pressed;

    (void)m;

    if (!cfg->enabled)
        return;

    pressed = (frame_count >= cfg->start_frame &&
               frame_count < cfg->start_frame + cfg->count);

    if (pressed == cfg->active)
        return;

    cfg->active = pressed;

    fprintf(stderr,
            "[HARNESS] Mouse RMB port %u %s at frame %ld\n",
            cfg->port, pressed ? "pressed" : "released", frame_count);
}

static void harness_load_mouse_serial_trigger(HarnessMouseSerialTrigger *cfg)
{
    const char *value = getenv("HARNESS_SERIAL_AFTER_RMB");
    const char *delay_str = getenv("HARNESS_SERIAL_AFTER_RMB_DELAY");
    const char *port_str = getenv("HARNESS_SERIAL_AFTER_RMB_PORT");
    char *end = NULL;
    uint8_t byte = 0;

    memset(cfg, 0, sizeof(*cfg));

    if (!value || !*value)
        return;

    if (!harness_parse_serial_byte(value, &byte)) {
        fprintf(stderr,
                "[HARNESS] Ignoring HARNESS_SERIAL_AFTER_RMB=%s (expected single char or 0xNN)\n",
                value);
        return;
    }

    cfg->byte = byte;
    cfg->delay_frames = 0;
    cfg->port = 0u;
    cfg->enabled = 1;

    if (delay_str && *delay_str) {
        cfg->delay_frames = strtol(delay_str, &end, 10);
        if (!end || *end != '\0' || cfg->delay_frames < 0) {
            fprintf(stderr,
                    "[HARNESS] Ignoring HARNESS_SERIAL_AFTER_RMB_DELAY=%s (expected non-negative frame delay)\n",
                    delay_str);
            cfg->delay_frames = 0;
        }
    }

    if (port_str && *port_str) {
        unsigned long port = strtoul(port_str, &end, 10);
        if (!end || *end != '\0' || port > 1u) {
            fprintf(stderr,
                    "[HARNESS] Ignoring HARNESS_SERIAL_AFTER_RMB_PORT=%s (expected 0 or 1)\n",
                    port_str);
        } else {
            cfg->port = (unsigned)port;
        }
    }
}

static int harness_map_host_key_to_amiga_raw(uint32_t host_key, uint8_t *rawkey)
{
    if (!rawkey)
        return 0;

    switch (host_key) {
    case PAL_HOST_KEY_1: *rawkey = 0x01u; return 1;
    case PAL_HOST_KEY_2: *rawkey = 0x02u; return 1;
    case PAL_HOST_KEY_3: *rawkey = 0x03u; return 1;
    case PAL_HOST_KEY_4: *rawkey = 0x04u; return 1;
    case PAL_HOST_KEY_5: *rawkey = 0x05u; return 1;
    case PAL_HOST_KEY_6: *rawkey = 0x06u; return 1;
    case PAL_HOST_KEY_7: *rawkey = 0x07u; return 1;
    case PAL_HOST_KEY_8: *rawkey = 0x08u; return 1;
    case PAL_HOST_KEY_9: *rawkey = 0x09u; return 1;
    case PAL_HOST_KEY_0: *rawkey = 0x0Au; return 1;
    case PAL_HOST_KEY_SPACE: *rawkey = 0x40u; return 1;
    case PAL_HOST_KEY_RETURN: *rawkey = 0x44u; return 1;
    case PAL_HOST_KEY_KP_ENTER: *rawkey = 0x43u; return 1;
    case PAL_HOST_KEY_ESCAPE: *rawkey = 0x45u; return 1;
    case PAL_HOST_KEY_UP: *rawkey = 0x4Cu; return 1;
    case PAL_HOST_KEY_DOWN: *rawkey = 0x4Du; return 1;
    case PAL_HOST_KEY_RIGHT: *rawkey = 0x4Eu; return 1;
    case PAL_HOST_KEY_LEFT: *rawkey = 0x4Fu; return 1;
    case PAL_HOST_KEY_KP_0: *rawkey = 0x0Fu; return 1;
    case PAL_HOST_KEY_KP_1: *rawkey = 0x1Du; return 1;
    case PAL_HOST_KEY_KP_2: *rawkey = 0x1Eu; return 1;
    case PAL_HOST_KEY_KP_3: *rawkey = 0x1Fu; return 1;
    case PAL_HOST_KEY_KP_4: *rawkey = 0x2Du; return 1;
    case PAL_HOST_KEY_KP_5: *rawkey = 0x2Eu; return 1;
    case PAL_HOST_KEY_KP_6: *rawkey = 0x2Fu; return 1;
    case PAL_HOST_KEY_KP_7: *rawkey = 0x3Du; return 1;
    case PAL_HOST_KEY_KP_8: *rawkey = 0x3Eu; return 1;
    case PAL_HOST_KEY_KP_9: *rawkey = 0x3Fu; return 1;
    default:
        return 0;
    }
}

static const char *harness_host_key_name(uint32_t host_key)
{
    switch (host_key) {
    case PAL_HOST_KEY_0: return "0";
    case PAL_HOST_KEY_1: return "1";
    case PAL_HOST_KEY_2: return "2";
    case PAL_HOST_KEY_3: return "3";
    case PAL_HOST_KEY_4: return "4";
    case PAL_HOST_KEY_5: return "5";
    case PAL_HOST_KEY_6: return "6";
    case PAL_HOST_KEY_7: return "7";
    case PAL_HOST_KEY_8: return "8";
    case PAL_HOST_KEY_9: return "9";
    case PAL_HOST_KEY_SPACE: return "SPACE";
    case PAL_HOST_KEY_RETURN: return "RETURN";
    case PAL_HOST_KEY_KP_ENTER: return "KP_ENTER";
    case PAL_HOST_KEY_ESCAPE: return "ESC";
    case PAL_HOST_KEY_UP: return "UP";
    case PAL_HOST_KEY_DOWN: return "DOWN";
    case PAL_HOST_KEY_LEFT: return "LEFT";
    case PAL_HOST_KEY_RIGHT: return "RIGHT";
    case PAL_HOST_KEY_KP_0: return "KP0";
    case PAL_HOST_KEY_KP_1: return "KP1";
    case PAL_HOST_KEY_KP_2: return "KP2";
    case PAL_HOST_KEY_KP_3: return "KP3";
    case PAL_HOST_KEY_KP_4: return "KP4";
    case PAL_HOST_KEY_KP_5: return "KP5";
    case PAL_HOST_KEY_KP_6: return "KP6";
    case PAL_HOST_KEY_KP_7: return "KP7";
    case PAL_HOST_KEY_KP_8: return "KP8";
    case PAL_HOST_KEY_KP_9: return "KP9";
    default:
        return "?";
    }
}

static void harness_pump_sdl_keyboard(void)
{
    PAL_KeyEvent event;

    while (pal_sdl_pop_key_event(&event)) {
        uint8_t rawkey = 0;

        if (!harness_map_host_key_to_amiga_raw(event.host_key, &rawkey))
            continue;

        if (!bellatrix_machine_keyboard_rawkey(rawkey, event.pressed)) {
            fprintf(stderr,
                    "[HARNESS] Keyboard queue full, dropped rawkey 0x%02x\n",
                    (unsigned)rawkey);
            continue;
        }

        fprintf(stderr,
                "[HARNESS] SDL key host=%s sc=%u key=%u -> rawkey=0x%02x %s\n",
                harness_host_key_name(event.host_key),
                (unsigned)event.scancode,
                (unsigned)event.keycode,
                (unsigned)rawkey,
                event.pressed ? "down" : "up");
    }
}

static void harness_pump_serial_rx(BellatrixMachine *m)
{
    uint8_t byte = 0;

    while (PAL_HarnessSerial_ReadByte(&byte)) {
        uart_receive_byte(&m->paula.uart, byte);
        fprintf(stderr,
                "[HARNESS] PTY serial RX -> UART 0x%02x%s\n",
                (unsigned)byte,
                (byte >= 32 && byte <= 126) ? "" : "");
    }
}

static void harness_update_mouse_serial_trigger(long frame_count,
                                                int mouse_right_down,
                                                HarnessMouseSerialTrigger *cfg)
{
    if (!cfg->enabled)
        return;

    if (mouse_right_down && !cfg->waiting_release && !cfg->pending.enabled) {
        memset(&cfg->pending, 0, sizeof(cfg->pending));
        cfg->pending.byte = cfg->byte;
        cfg->pending.frame = frame_count + cfg->delay_frames;
        cfg->pending.enabled = 1;
        cfg->waiting_release = 1;

        fprintf(stderr,
                "[HARNESS] Armed serial after RMB: frame=%ld byte=0x%02x\n",
                cfg->pending.frame, (unsigned)cfg->pending.byte);
        return;
    }

    if (!mouse_right_down)
        cfg->waiting_release = 0;
}

/* ---------------------------------------------------------------------------
 * File loading
 * ------------------------------------------------------------------------- */

static uint8_t *load_file_limited(const char *path,
                                  uint32_t *out_size,
                                  uint32_t max_size,
                                  const char *kind)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0 || (uint32_t)sz > max_size) {
        fprintf(stderr, "%s size out of range: %ld bytes\n", kind, sz);
        fclose(f);
        return NULL;
    }

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fputs("OOM\n", stderr); fclose(f); return NULL; }

    if ((long)fread(buf, 1, (size_t)sz, f) != sz) {
        perror("fread");
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = (uint32_t)sz;
    return buf;
}

/* ---------------------------------------------------------------------------
 * Chip RAM — static host buffer (replaces Emu68 kernel-virtual address)
 * ------------------------------------------------------------------------- */

static uint8_t s_chip_ram[2 * 1024 * 1024];

static void harness_memory_init(BellatrixMemory *m)
{
    /* Match the reference emulator's default chip RAM pattern.  KS1.3/AROS
     * are sensitive to the power-on contents of low memory. */
    memset(s_chip_ram, 0x84, sizeof(s_chip_ram));
    memset(s_chip_ram, 0xFF, 0x040000);

    /* Keep Fast RAM / ROM / overlay state from bellatrix_machine_init().
     * The harness only replaces the chip RAM backing with a host buffer. */
    m->chip_ram      = s_chip_ram;
    m->chip_ram_size = sizeof(s_chip_ram);
    m->chip_ram_mask = sizeof(s_chip_ram) - 1u;
}

/* ---------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    harness_install_signal_handlers();
    PAL_HarnessSerial_ConfigureFromEnv();

    const char *rom_path    = NULL;
    const char *adf_path    = NULL;
    int         headless    = 0;
    long        max_cycles  = 0;
    long        max_frames  = 0;

    uint8_t    *adf_data    = NULL;
    uint32_t    adf_size    = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) {
            headless = 1;
        } else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
            max_cycles = atol(argv[++i]);
            headless   = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atol(argv[++i]);
            headless   = 1;
        } else if (strcmp(argv[i], "--adf") == 0 && i + 1 < argc) {
            adf_path = argv[++i];
        } else if (argv[i][0] != '-') {
            rom_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr,
                "Usage: harness <rom.bin> [--adf disk.adf] [--headless] [--cycles N] [--frames N]\n");
            return 1;
        }
    }

    if (!rom_path) {
        fprintf(stderr,
            "Usage: harness <rom.bin> [--adf disk.adf] [--headless] [--cycles N] [--frames N]\n");
        return 1;
    }

    /* Load ROM */
    uint32_t rom_size = 0;
    uint8_t *rom_data = load_file_limited(rom_path, &rom_size, 1024u * 1024u, "ROM");
    if (!rom_data) return 1;

    uint32_t rom_base = (rom_size <= 256u * 1024u) ? 0xFC0000u : 0xF80000u;
    const char *layout = (rom_size > 512u * 1024u)
                         ? "1MB (0xE00000+0xF80000)"
                         : (rom_size <= 256u * 1024u ? "256KB@0xFC0000" : "512KB@0xF80000");
    printf("[HARNESS] ROM: %s  size=%u KB  layout=%s\n",
           rom_path, rom_size / 1024u, layout);
    printf("[HARNESS] Serial mode: %s\n", PAL_HarnessSerial_ModeName());

    uint32_t std_off = (rom_size > 512u * 1024u) ? rom_size / 2u : 0u;
    if (rom_size >= std_off + 8u) {
        const uint8_t *rv = rom_data + std_off;
        uint32_t isp = ((uint32_t)rv[0] << 24) | ((uint32_t)rv[1] << 16) |
                       ((uint32_t)rv[2] <<  8) |  (uint32_t)rv[3];
        uint32_t pc  = ((uint32_t)rv[4] << 24) | ((uint32_t)rv[5] << 16) |
                       ((uint32_t)rv[6] <<  8) |  (uint32_t)rv[7];
        printf("[HARNESS] Reset vectors: ISP=0x%08x  PC=0x%08x\n", isp, pc);
        if (pc < 0xE00000u || pc > 0xFFFFFFu)
            fprintf(stderr,
                "[HARNESS] WARNING: PC 0x%08x outside ROM range\n", pc);
    }

    /* Load ADF, if provided */
    if (adf_path) {
        adf_data = load_file_limited(adf_path, &adf_size, 2u * 1024u * 1024u, "ADF");
        if (!adf_data) {
            free(rom_data);
            return 1;
        }

        printf("[HARNESS] ADF: %s  size=%u bytes\n", adf_path, adf_size);

        if (adf_size != 901120u) {
            fprintf(stderr,
                    "[HARNESS] WARNING: unusual ADF size: %u bytes, expected 901120\n",
                    adf_size);
        }
    }

    /* Init display before machine so framebuffer globals are set */
    if (!headless) {
        if (PAL_Video_Init(640, 512, 16) != 0) {
            fprintf(stderr,
                "[HARNESS] SDL2 unavailable — falling back to headless\n");
            headless = 1;
        }
    }

    harness_wait_for_serial_attach();

    /* Init Musashi + load ROM */
    musashi_backend_init();
    musashi_backend_load_rom(rom_data, rom_size, rom_base);
    free(rom_data);

    /* Init machine.  bellatrix_machine_init() sets chip_ram to the Emu68
     * kernel-virtual address — overwrite immediately with the host buffer. */
    bellatrix_machine_init(musashi_backend_get());
    BellatrixMachine *m = bellatrix_machine_get();
    harness_memory_init(&m->memory);

    /* Re-wire Paula disk DMA to the harness chip RAM buffer.
     * machine_init wires Paula before harness_memory_init replaces the pointer. */
    paula_attach_memory(&m->paula, m->memory.chip_ram, m->memory.chip_ram_size);

    /* Mount or eject DF0 explicitly. */
    if (adf_data) {
        if (!bellatrix_machine_insert_df0_adf(adf_data, adf_size)) {
            fprintf(stderr, "[HARNESS] Failed to insert ADF into DF0\n");
            free(adf_data);
            return 1;
        }
    } else {
        bellatrix_machine_eject_df0();
    }

    /* CIA-A defaults: OVL and LED are outputs */
    m->cia_a.ddra = 0x03u;

    /* Pulse CPU reset — reads ISP+PC from bus through overlay */
    musashi_backend_reset();

    if (headless) {
        printf("[HARNESS] Headless mode  max_cycles=%ld  max_frames=%ld\n",
               max_cycles, max_frames);
    } else {
        printf("[HARNESS] Interactive mode — close window or press Esc to quit\n");
    }

    /* ---------------------------------------------------------------------------
     * Main loop
     * ------------------------------------------------------------------------- */

    const int QUANTUM = 454;

    long  total_cycles = 0;
    long  frame_count  = 0;
    long  prev_frame   = -1;
    int   running      = 1;
    HarnessSerialInject serial_bootkey;
    HarnessSerialInject serial_menu;
    HarnessSerialInject serial_script[HARNESS_MAX_SERIAL_SCRIPT + HARNESS_MAX_SERIAL_HOLD];
    int serial_script_count = 0;
    HarnessMouseHold mouse_rmb;
    HarnessMouseSerialTrigger mouse_serial_trigger;

    uint64_t fps_t0    = PAL_Time_ReadCounter();
    long     fps_frames = 0;
    char     title_buf[128];

    harness_load_serial_inject("HARNESS_SERIAL_BOOTKEY",
                               "HARNESS_SERIAL_BOOTKEY_FRAME",
                               5,
                               &serial_bootkey);
    harness_load_serial_inject("HARNESS_SERIAL_INJECT",
                               "HARNESS_SERIAL_INJECT_FRAME",
                               300,
                               &serial_menu);
    serial_script_count = harness_load_serial_script(
        serial_script, HARNESS_MAX_SERIAL_SCRIPT + HARNESS_MAX_SERIAL_HOLD);
    serial_script_count += harness_append_serial_hold(
        serial_script, (size_t)serial_script_count,
        HARNESS_MAX_SERIAL_SCRIPT + HARNESS_MAX_SERIAL_HOLD);
    harness_load_mouse_hold(&mouse_rmb);
    harness_load_mouse_serial_trigger(&mouse_serial_trigger);

    while (running) {
        int sdl_mouse_right = 0;

        if (!headless) {
            running = pal_sdl_poll_events();
            if (!running) break;
            sdl_mouse_right = pal_sdl_mouse_right_down();
            harness_pump_sdl_keyboard();
        }

        paula_set_mouse_right(&m->paula, 0u,
                              sdl_mouse_right ||
                              (mouse_rmb.enabled && mouse_rmb.port == 0u && mouse_rmb.active));
        paula_set_mouse_right(&m->paula, 1u,
                              (mouse_rmb.enabled && mouse_rmb.port == 1u && mouse_rmb.active));
        harness_update_mouse_serial_trigger(frame_count, sdl_mouse_right,
                                            &mouse_serial_trigger);
        harness_pump_serial_rx(m);

        int used = musashi_backend_run(QUANTUM);
        bellatrix_machine_advance((uint32_t)used);
        total_cycles += used;

        long cur_frame = (long)bellatrix_machine_agnus()->beam.frame;
        if (cur_frame != prev_frame) {
            frame_count++;
            fps_frames++;
            prev_frame = cur_frame;

            harness_update_mouse_hold(m, frame_count, &mouse_rmb);
            harness_maybe_inject_serial(m, frame_count, &serial_bootkey, "bootkey");
            harness_maybe_inject_serial(m, frame_count, &serial_menu, "input");
            harness_maybe_inject_serial(m, frame_count, &mouse_serial_trigger.pending,
                                        "after-rmb");
            harness_maybe_log_serial_consumed(m, frame_count, &serial_bootkey, "bootkey");
            harness_maybe_log_serial_consumed(m, frame_count, &serial_menu, "input");
            harness_maybe_log_serial_consumed(m, frame_count,
                                              &mouse_serial_trigger.pending,
                                              "after-rmb");
            for (int i = 0; i < serial_script_count; i++) {
                char label[32];
                snprintf(label, sizeof(label), "script[%d]", i);
                harness_maybe_inject_serial(m, frame_count, &serial_script[i], label);
                harness_maybe_log_serial_consumed(m, frame_count, &serial_script[i], label);
            }

            if (!headless) {
                uint64_t now  = PAL_Time_ReadCounter();
                uint64_t freq = PAL_Time_GetFrequency();
                if (now - fps_t0 >= freq) {
                    double fps = (double)fps_frames * (double)freq /
                                 (double)(now - fps_t0);
                    snprintf(title_buf, sizeof(title_buf),
                             "Bellatrix  %.1f fps  frame=%ld", fps, frame_count);
                    pal_sdl_set_title(title_buf);
                    fps_frames = 0;
                    fps_t0     = now;
                }
            }

            if (headless) {
                if (max_frames > 0 && frame_count >= max_frames) {
                    printf("[HARNESS] Reached %ld frames — stopping\n",
                           frame_count);
                    break;
                }
            }

            if (frame_count == 4000) {
                const uint8_t *cr = m->memory.chip_ram;
                uint32_t eb = ((uint32_t)cr[4] << 24) | ((uint32_t)cr[5] << 16) |
                              ((uint32_t)cr[6] << 8) | (uint32_t)cr[7];
                printf("[EXEC-DUMP] frame=%ld ExecBase=0x%08x  PC=0x%08x\n",
                       frame_count, (unsigned)eb,
                       (unsigned)m68k_get_reg(NULL, M68K_REG_PC));
                if (eb >= 4 && eb + 0x400 < m->memory.chip_ram_size) {
                    printf("[EXEC-DUMP] eb+0x160:");
                    for (int i = 0; i < 64; i++) printf(" %02x", cr[eb + 0x160 + i]);
                    printf("\n[EXEC-DUMP] eb+0x1a0:");
                    for (int i = 0; i < 64; i++) printf(" %02x", cr[eb + 0x1a0 + i]);
                    printf("\n[EXEC-DUMP] eb+0x270:");
                    for (int i = 0; i < 32; i++) printf(" %02x", cr[eb + 0x270 + i]);
                    printf("\n[EXEC-DUMP] eb+0x120:");
                    for (int i = 0; i < 16; i++) printf(" %02x", cr[eb + 0x120 + i]);
                    printf("\n");
                    uint32_t head = 0x24CC;
                    if (head + 64 < m->memory.chip_ram_size) {
                        printf("[EXEC-DUMP] 0x24CC:");
                        for (int i = 0; i < 64; i++) printf(" %02x", cr[head + i]);
                        printf("\n");
                    }
                    printf("[EXEC-DUMP] A4=0x%08x  D0=0x%08x\n",
                           (unsigned)m68k_get_reg(NULL, M68K_REG_A4),
                           (unsigned)m68k_get_reg(NULL, M68K_REG_D0));
                }
            }
        }

        if (headless && max_cycles > 0 && total_cycles >= max_cycles) {
            printf("[HARNESS] Reached %ld cycles — stopping\n", total_cycles);
            break;
        }
    }

    printf("[HARNESS] Done.  cycles=%ld  frames=%ld  PC=0x%08x\n",
           total_cycles, frame_count,
           (unsigned)m68k_get_reg(NULL, M68K_REG_PC));

    free(adf_data);

    return 0;
}
