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
    memset(m, 0, sizeof(*m));
    memset(s_chip_ram, 0, sizeof(s_chip_ram));
    m->chip_ram      = s_chip_ram;
    m->chip_ram_size = sizeof(s_chip_ram);
    m->chip_ram_mask = sizeof(s_chip_ram) - 1u;
}

/* ---------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
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

    uint64_t fps_t0    = PAL_Time_ReadCounter();
    long     fps_frames = 0;
    char     title_buf[128];

    while (running) {
        if (!headless) {
            running = pal_sdl_poll_events();
            if (!running) break;
        }

        int used = musashi_backend_run(QUANTUM);
        bellatrix_machine_advance((uint32_t)used);
        total_cycles += used;

        long cur_frame = (long)bellatrix_machine_agnus()->beam.frame;
        if (cur_frame != prev_frame) {
            frame_count++;
            fps_frames++;
            prev_frame = cur_frame;

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