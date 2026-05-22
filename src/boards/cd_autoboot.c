// src/boards/cd_autoboot.c
//
// Host-side expansion board: registers a pre-compiled m68k ROM as a Zorro II
// Autoconfig board at 0xE80000.  AROS's expansion library reads the DiagArea
// at board_base+0x0010, CopyMem's it to AllocMem, and calls da_DiagPoint
// which creates a background task that mounts the CD once dos.library is up.

#include "cd_autoboot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus/zorro2/zorro2_bus.h"

static uint8_t *s_buf;

int cd_autoboot_init(BellatrixMachine *m, const char *rom_path)
{
    (void)m;

    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        fprintf(stderr, "[CD-BOARD] '%s' not found — CD automount disabled\n", rom_path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_sz <= 0 || (size_t)file_sz > AUTOCONFIG_CD_BOARD_WINDOW) {
        fprintf(stderr, "[CD-BOARD] '%s': invalid size %ld\n", rom_path, file_sz);
        fclose(f);
        return -1;
    }

    s_buf = malloc((size_t)file_sz);
    if (!s_buf) { fclose(f); return -1; }

    size_t n = fread(s_buf, 1, (size_t)file_sz, f);
    fclose(f);

    bellatrix_zorro2_enable_cd_board(s_buf, n);
    fprintf(stderr, "[CD-BOARD] ROM %zu B → Autoconfig Z2 DiagArea @ er_InitDiagVec=0x0010\n", n);
    return 0;
}

void cd_autoboot_free(void)
{
    free(s_buf);
    s_buf = NULL;
}
