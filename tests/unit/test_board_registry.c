/*
 * Self-registering board table: discovery -> autoconfig -> map -> access.
 *
 * Proves, on the POSIX host, the exact registration mechanism the product uses:
 * boards drop a descriptor into a linker section (BELLATRIX_REGISTER_BOARD_*)
 * and are discovered by walking __start_/__stop_ boundary symbols — no central
 * register_board() call, no linker script. The autoconfig walker then presents
 * each board at $E80000, and the guest-written base completes assignment and
 * fires the board's map(), which installs a DIRECT region reachable afterwards.
 *
 * Two boards are dropped below (a Z2 RAM board, then a Z3 ROM board). Because
 * they are compiled directly into this test executable they self-register; the
 * whole point is that no code here calls a register function.
 */
#include "cpu/direct_region.h"
#include "machine/autoconfig/autoconfig.h"
#include "machine/bus/board_registry.h"
#include "machine/memory/memory.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int kprintf(const char *fmt, ...)
{
    (void)fmt;
    return 0;
}

static void check_eq(const char *name, uint32_t expected, uint32_t actual)
{
    if (expected != actual) {
        fprintf(stderr, "FAIL %s expected=%08x actual=%08x\n",
                name, (unsigned)expected, (unsigned)actual);
        exit(1);
    }
}

/* One DIRECT map shared by the test board map() callbacks. A real backend would
 * install MMU/bank; here a probe records the region so we can read it back. */
static BellatrixDirectRegionMap s_direct_map;

static int probe_map(void *opaque, const BellatrixDirectRegion *region)
{
    (void)opaque;
    (void)region;
    return 0;
}

static int probe_unmap(void *opaque, const BellatrixDirectRegion *region)
{
    (void)opaque;
    (void)region;
    return 0;
}

static const BellatrixDirectRegionBackendOps s_direct_ops = {
    .map = probe_map,
    .unmap = probe_unmap,
};

/* ---- board 0: Zorro II RAM (type Z2), 64 KiB window ------------------------ */

static uint8_t s_z2_ram[0x1000] __attribute__((aligned(0x1000)));
static uint8_t s_z2_ac[AUTOCONFIG_DATA_SIZE];
static int     s_z2_mapped;

static void z2_ram_map(BellatrixBoard *board)
{
    BellatrixDirectRegion region = {
        .guest_base = board->map_base,
        .size = sizeof(s_z2_ram),
        .host_base = s_z2_ram,
        .flags = BELLATRIX_DIRECT_READ | BELLATRIX_DIRECT_WRITE |
                 BELLATRIX_DIRECT_CACHEABLE,
    };
    if (bellatrix_direct_region_install(&s_direct_map, &region) == 0)
        s_z2_mapped = 1;
}

static BellatrixBoard s_z2_board = {
    .rom_file = s_z2_ac,
    .rom_size = sizeof(s_z2_ram),
    .is_z3 = 0u,
    .enabled = 1u,
    .map = z2_ram_map,
};
BELLATRIX_REGISTER_BOARD_Z2(s_z2_board);

/* ---- board 1: Zorro III ROM (type Z3), 4 KiB read-only -------------------- */

static uint8_t s_z3_rom[0x1000] __attribute__((aligned(0x1000)));
static uint8_t s_z3_ac[AUTOCONFIG_DATA_SIZE];
static int     s_z3_mapped;

static void z3_rom_map(BellatrixBoard *board)
{
    BellatrixDirectRegion region = {
        .guest_base = board->map_base,
        .size = sizeof(s_z3_rom),
        .host_base = s_z3_rom,
        .flags = BELLATRIX_DIRECT_READ | BELLATRIX_DIRECT_EXECUTE |
                 BELLATRIX_DIRECT_CACHEABLE,
    };
    if (bellatrix_direct_region_install(&s_direct_map, &region) == 0)
        s_z3_mapped = 1;
}

static BellatrixBoard s_z3_board = {
    .rom_file = s_z3_ac,
    .rom_size = sizeof(s_z3_rom),
    .is_z3 = 1u,
    .enabled = 1u,
    .map = z3_rom_map,
};
BELLATRIX_REGISTER_BOARD_Z3(s_z3_board);

int main(void)
{
    const BellatrixDirectRegion *region;
    uint32_t value = 0u;

    /* Autoconfig images: first byte carries the board type nibble the guest
     * reads back at $E80000. Backing content is a marker to prove access. */
    memset(s_z2_ac, 0, sizeof(s_z2_ac));
    memset(s_z3_ac, 0, sizeof(s_z3_ac));
    s_z2_ac[0] = AC_TYPE_Z2;
    s_z3_ac[0] = AC_TYPE_Z3;
    s_z2_ram[0] = 0x5au;
    s_z3_rom[0] = 0xa5u;

    bellatrix_direct_region_map_init(&s_direct_map, &s_direct_ops, NULL);

    /* --- Discovery: both boards self-registered, in link order. --- */
    check_eq("two boards discovered", 2u, (uint32_t)bellatrix_board_count());
    check_eq("board 0 is the Z2 board", (uint32_t)(uintptr_t)&s_z2_board,
             (uint32_t)(uintptr_t)bellatrix_board_at(0));
    check_eq("board 1 is the Z3 board", (uint32_t)(uintptr_t)&s_z3_board,
             (uint32_t)(uintptr_t)bellatrix_board_at(1));

    bellatrix_boards_autoconfig_reset();

    /* --- Autoconfig board 0 (Z2): config read, then base assign fires map. --- */
    check_eq("z2 presented first", AC_TYPE_Z2,
             bellatrix_boards_autoconfig_read8(BELLATRIX_Z2_CONFIG_BASE));
    check_eq("z2 not mapped before base", 0u, (uint32_t)s_z2_mapped);
    bellatrix_boards_autoconfig_write(BELLATRIX_Z2_CONFIG_BASE + AC_OFF_BASE_HI,
                                      0x20u, 1u); /* base = 0x20 << 16 */
    check_eq("z2 map fired", 1u, (uint32_t)s_z2_mapped);
    region = bellatrix_direct_region_find(&s_direct_map, 0x00200000u, 1u);
    check_eq("z2 region installed at guest base", 1u, region != NULL);

    /* --- Walk advanced: board 1 (Z3) is now presented. --- */
    check_eq("z3 presented after z2", AC_TYPE_Z3,
             bellatrix_boards_autoconfig_read8(BELLATRIX_Z2_CONFIG_BASE));
    bellatrix_boards_autoconfig_write(BELLATRIX_Z2_CONFIG_BASE + AC_OFF_Z3_HI,
                                      0x4000u, 2u); /* base = 0x4000 << 16 */
    check_eq("z3 map fired", 1u, (uint32_t)s_z3_mapped);
    region = bellatrix_direct_region_find(&s_direct_map, 0x40000000u, 1u);
    check_eq("z3 region installed at guest base", 1u, region != NULL);
    check_eq("z3 region is read-only", 0u,
             region->flags & BELLATRIX_DIRECT_WRITE);

    /* --- Table exhausted: nothing more to present. --- */
    check_eq("table exhausted", 0xffu,
             bellatrix_boards_autoconfig_read8(BELLATRIX_Z2_CONFIG_BASE));

    /* --- Access: the mapped backing is reachable at the assigned bases. --- */
    check_eq("z2 backing readable", 1u,
             (uint32_t)bellatrix_direct_region_read(&s_direct_map, 0x00200000u,
                                                    1u, &value));
    check_eq("z2 backing content", 0x5au, value);
    check_eq("z3 backing readable", 1u,
             (uint32_t)bellatrix_direct_region_read(&s_direct_map, 0x40000000u,
                                                    1u, &value));
    check_eq("z3 backing content", 0xa5u, value);

    /* --- Reset rewinds the walk to the first board. --- */
    bellatrix_boards_autoconfig_reset();
    check_eq("reset rewinds to z2", AC_TYPE_Z2,
             bellatrix_boards_autoconfig_read8(BELLATRIX_Z2_CONFIG_BASE));

    puts("board registry tests passed");
    return 0;
}
