/*
 * Super Buster Z3 decode over a board_registry EXTERNAL board.
 *
 * A Zorro III board registered through the self-registering board_registry
 * (map == NULL) is served per access — no host MMU/bank backing — so its window
 * must be decoded by the Super Buster. The chain is: enable board -> Autoconfig
 * assigns the guest base (word at offset 0x44) -> the Super Buster decodes
 * 32-bit CPU-space accesses to that window. The decode is gated on the Buster's
 * own NBSTAB ("Z3 bus available") bit.
 */
#include "machine/autoconfig/autoconfig.h"
#include "machine/bus/board_registry.h"
#include "machine/bus/superbuster/superbuster.h"

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

/* EXTERNAL Zorro III board: map == NULL, a 128KB window served per access. */
static uint8_t s_reg_config[AUTOCONFIG_DATA_SIZE];
static struct ExpansionBoard s_reg_board = {
    .rom_file = s_reg_config,
    .rom_size = 0x00020000u,
    .map_base = 0u,
    .is_z3    = 1u,
    .enabled  = 1u,
    .map      = NULL,
};
BELLATRIX_REGISTER_BOARD_Z3(s_reg_board);

int main(void)
{
    SuperBusterState sb;

    s_reg_config[0] = 0x80u; /* Zorro III er_Type (config-read content only) */

    superbuster_init(&sb);
    check_eq("NBSTAB set after init", SUPERBUSTER_NBSTAB,
             sb.ctrl & SUPERBUSTER_NBSTAB);

    /* Before Autoconfig assigns a base, nothing is decoded. */
    bellatrix_boards_autoconfig_reset();
    check_eq("undecoded before autoconfig", (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x40000000u));

    /* Guest assigns base[31:16] via the word at Z3 offset 0x44. */
    bellatrix_boards_autoconfig_write(0x00e80044u, 0x4000u, 2u);
    check_eq("base latched", 0x40000000u, s_reg_board.map_base);

    /* Super Buster now decodes the whole window as an EXTERNAL board slot. */
    check_eq("decode window start", (uint32_t)SUPERBUSTER_Z3_BOARD,
             (uint32_t)superbuster_decode_z3(&sb, 0x40000000u));
    check_eq("decode window end", (uint32_t)SUPERBUSTER_Z3_BOARD,
             (uint32_t)superbuster_decode_z3(&sb, 0x4001ffffu));
    check_eq("decode just past window", (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x40020000u));
    check_eq("decode below window", (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x3fffffffu));

    /* NBSTAB gating: the Buster refuses to answer for any slot when the Z3 bus
     * is reported unavailable, even for a configured window. */
    sb.ctrl &= (uint8_t)~SUPERBUSTER_NBSTAB;
    check_eq("no decode when Z3 bus disabled",
             (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x40000000u));
    sb.ctrl |= SUPERBUSTER_NBSTAB;

    /* A shutup at 0x4C rewinds the board out of the walk and clears its base. */
    bellatrix_boards_autoconfig_reset();
    bellatrix_boards_autoconfig_write(0x00e8004cu, 0x00u, 1u);
    check_eq("base cleared on shutup", 0u, s_reg_board.map_base);
    check_eq("no decode after shutup", (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x40000000u));

    puts("superbuster z3 tests passed");
    return 0;
}
