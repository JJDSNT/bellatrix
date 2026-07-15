#include "machine/expansions/z3_68040/z3_68040.h"

#include "cpu/direct_region.h"
#include "machine/autoconfig/autoconfig.h"
#include "machine/bus/zorro3/zorro3.h"
#include "support.h"

#include <stdint.h>
#include <string.h>

/* Reuse the exact 4 KiB ROM shipped by Emu68's native 68040 Z3 board. Its
 * first 64 bytes are the nibble-wide Autoconfig image and its mapped DiagArea
 * begins at offset 0x80. Renaming the generated symbol keeps this adapter
 * independent from whether native Emu68 boards are also compiled. */
#define __68040_bin bellatrix_z3_68040_rom
__attribute__((aligned(BELLATRIX_DIRECT_PAGE_SIZE)))
#include "boards/68040.h"
#undef __68040_bin

#define BELLATRIX_Z3_68040_WINDOW_SIZE 0x00010000u

typedef struct BellatrixZ368040 {
    CpuBackend *cpu_backend;
    uint32_t mapped_base;
} BellatrixZ368040;

static BellatrixZ368040 s_board;

static int z3_68040_map(void *userdata, uint32_t base, uint32_t window_size)
{
    BellatrixZ368040 *board = (BellatrixZ368040 *)userdata;
    BellatrixDirectRegion rom = {
        .guest_base = base,
        .size = sizeof(bellatrix_z3_68040_rom),
        .host_base = bellatrix_z3_68040_rom,
        .flags = BELLATRIX_DIRECT_READ | BELLATRIX_DIRECT_EXECUTE |
                 BELLATRIX_DIRECT_CACHEABLE,
    };
    int rc;

    if (!board || !board->cpu_backend || window_size < rom.size)
        return -1;
    rc = cpu_backend_map_direct(board->cpu_backend, &rom);
    if (rc != 0)
        return rc;
    board->mapped_base = base;
    kprintf("[Z3-68040] mapped native Emu68 ROM base=%08x size=%08x\n",
            (unsigned)base, (unsigned)rom.size);
    return 0;
}

static void z3_68040_unmap(void *userdata, uint32_t base,
                           uint32_t window_size)
{
    BellatrixZ368040 *board = (BellatrixZ368040 *)userdata;
    int rc;

    (void)window_size;
    if (!board || !board->cpu_backend || board->mapped_base != base)
        return;
    rc = cpu_backend_unmap_direct(board->cpu_backend, base,
                                  sizeof(bellatrix_z3_68040_rom));
    if (rc == 0) {
        board->mapped_base = 0u;
        kprintf("[Z3-68040] unmapped base=%08x\n", (unsigned)base);
    } else {
        kprintf("[Z3-68040] unmap failed rc=%d base=%08x\n", rc,
                (unsigned)base);
    }
}

int bellatrix_z3_68040_register(CpuBackend *cpu_backend)
{
    static const BellatrixZorro3BoardOps ops = {
        .map = z3_68040_map,
        .unmap = z3_68040_unmap,
    };
    static const BellatrixZorro3BoardDesc desc = {
        .id = "emu68.68040",
        .config_data = bellatrix_z3_68040_rom,
        .config_size = AUTOCONFIG_DATA_SIZE,
        .window_size = BELLATRIX_Z3_68040_WINDOW_SIZE,
        .userdata = &s_board,
        .ops = &ops,
    };

    _Static_assert(sizeof(bellatrix_z3_68040_rom) ==
                       BELLATRIX_DIRECT_PAGE_SIZE,
                   "Emu68 68040 support ROM must occupy one MMU page");
    memset(&s_board, 0, sizeof(s_board));
    s_board.cpu_backend = cpu_backend;
    return bellatrix_zorro3_register_board(&desc);
}
