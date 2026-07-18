/*
 * Super Buster Z3 decode + EXTERNAL board register window.
 *
 * Proves the shared spine for a Zorro III board whose registers are served by
 * per-access callbacks (EXTERNAL), as opposed to DIRECT RAM/ROM backing which
 * the CPU backend maps and which never reaches the Super Buster decode. The
 * chain is: register board -> Autoconfig assigns the guest base -> the board's
 * map() publishes its window -> the Super Buster decodes 32-bit CPU-space
 * accesses to that slot -> reads/writes reach the board callbacks. The decode
 * is also gated on the Buster's own NBSTAB ("Z3 bus available") bit.
 */
#include "machine/autoconfig/autoconfig.h"
#include "machine/bus/board_registry.h"
#include "machine/bus/superbuster/superbuster.h"
#include "machine/bus/zorro3/zorro3.h"

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

/* EXTERNAL board: a small register file addressed by the guest, with no host
 * MMU/bank backing — every access is served by these callbacks. */
typedef struct ExtBoard {
    uint8_t  regs[0x100];
    int      mapped;
    uint32_t base;
} ExtBoard;

static ExtBoard s_ext;

/* Second path under test: an EXTERNAL Zorro III board registered through the
 * self-registering board_registry (map == NULL). The Super Buster decode must
 * recognise its window too, not only legacy zorro3 registry boards. */
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

static int ext_map(void *userdata, uint32_t base, uint32_t size)
{
    ExtBoard *b = (ExtBoard *)userdata;
    (void)size;
    b->mapped = 1;
    b->base = base;
    return 0;
}

static void ext_unmap(void *userdata, uint32_t base, uint32_t size)
{
    ExtBoard *b = (ExtBoard *)userdata;
    (void)base;
    (void)size;
    b->mapped = 0;
}

static uint8_t ext_read8(void *userdata, uint32_t offset)
{
    ExtBoard *b = (ExtBoard *)userdata;
    return offset < sizeof(b->regs) ? b->regs[offset] : 0xffu;
}

static void ext_write8(void *userdata, uint32_t offset, uint8_t value)
{
    ExtBoard *b = (ExtBoard *)userdata;
    if (offset < sizeof(b->regs))
        b->regs[offset] = value;
}

int main(void)
{
    static uint8_t z3_config[AUTOCONFIG_DATA_SIZE];
    BellatrixZorro3BoardDesc z3;
    BellatrixZorro3BoardOps z3_ops;
    SuperBusterState sb;

    memset(z3_config, 0, sizeof(z3_config));
    /* er_Type: Zorro III board (encoding only exercises the config reads; the
     * decode under test keys off the assigned base/window, not this image). */
    z3_config[0] = 0x80u;

    memset(&z3, 0, sizeof(z3));
    memset(&z3_ops, 0, sizeof(z3_ops));
    memset(&s_ext, 0, sizeof(s_ext));
    z3_ops.map = ext_map;
    z3_ops.unmap = ext_unmap;
    z3_ops.read8 = ext_read8;
    z3_ops.write8 = ext_write8;
    z3.id = "test.z3.external";
    z3.config_data = z3_config;
    z3.config_size = sizeof(z3_config);
    z3.window_size = 0x00010000u;
    z3.userdata = &s_ext;
    z3.ops = &z3_ops;

    superbuster_init(&sb);
    check_eq("NBSTAB set after init", SUPERBUSTER_NBSTAB,
             sb.ctrl & SUPERBUSTER_NBSTAB);

    check_eq("register external z3", 0u,
             (uint32_t)bellatrix_zorro3_register_board(&z3));
    bellatrix_zorro3_init();

    /* Before Autoconfig assigns a base, nothing is decoded. */
    check_eq("undecoded before autoconfig", (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x40000000u));

    /* Guest assigns base[31:16] via the word at offset 0x44. */
    bellatrix_zorro3_config_write16(0x00e80044u, 0x4000u);
    check_eq("board configured", 1u,
             (uint32_t)bellatrix_zorro3_board_configured("test.z3.external"));
    check_eq("guest-assigned base", 0x40000000u,
             bellatrix_zorro3_board_base("test.z3.external"));
    check_eq("map callback fired", 1u, (uint32_t)s_ext.mapped);

    /* Super Buster now decodes the whole window as an EXTERNAL board slot. */
    check_eq("decode window start", (uint32_t)SUPERBUSTER_Z3_BOARD,
             (uint32_t)superbuster_decode_z3(&sb, 0x40000000u));
    check_eq("decode window end", (uint32_t)SUPERBUSTER_Z3_BOARD,
             (uint32_t)superbuster_decode_z3(&sb, 0x4000ffffu));
    check_eq("decode just past window", (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x40010000u));
    check_eq("decode below window", (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x3fffffffu));

    /* NBSTAB gating: the Buster refuses to answer for any slot when the Z3 bus
     * is reported unavailable, even for a configured window. */
    sb.ctrl &= (uint8_t)~SUPERBUSTER_NBSTAB;
    check_eq("no decode when Z3 bus disabled",
             (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x40000000u));
    sb.ctrl |= SUPERBUSTER_NBSTAB;

    /* EXTERNAL register accesses reach the board callbacks, big-endian. */
    bellatrix_zorro3_board_write8(0x40000010u, 0xabu);
    check_eq("write8 reached board", 0xabu, (uint32_t)s_ext.regs[0x10]);
    check_eq("read8 from board", 0xabu,
             (uint32_t)bellatrix_zorro3_board_read8(0x40000010u));

    bellatrix_zorro3_board_write16(0x40000020u, 0x1234u);
    check_eq("write16 hi byte", 0x12u, (uint32_t)s_ext.regs[0x20]);
    check_eq("write16 lo byte", 0x34u, (uint32_t)s_ext.regs[0x21]);
    check_eq("read16 assembles big-endian", 0x1234u,
             (uint32_t)bellatrix_zorro3_board_read16(0x40000020u));

    bellatrix_zorro3_board_write32(0x40000030u, 0xdeadbeefu);
    check_eq("read32 assembles big-endian", 0xdeadbeefu,
             bellatrix_zorro3_board_read32(0x40000030u));

    /* board_registry path: a Z3 EXTERNAL board drives the same decode. Before
     * its base is assigned, its window is open bus. */
    s_reg_config[0] = 0x80u; /* Zorro III er_Type (config-read content only) */
    bellatrix_boards_autoconfig_reset();
    check_eq("registry board undecoded before base",
             (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x50000000u));

    /* Guest assigns base[31:16] at Z3 offset 0x44 through the registry walker. */
    bellatrix_boards_autoconfig_write(0x00e80044u, 0x5000u, 2u);
    check_eq("registry board base latched", 0x50000000u, s_reg_board.map_base);
    check_eq("registry window start decoded", (uint32_t)SUPERBUSTER_Z3_BOARD,
             (uint32_t)superbuster_decode_z3(&sb, 0x50000000u));
    check_eq("registry window end decoded", (uint32_t)SUPERBUSTER_Z3_BOARD,
             (uint32_t)superbuster_decode_z3(&sb, 0x5001ffffu));
    check_eq("registry just past window", (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x50020000u));

    /* NBSTAB gates the registry path exactly as the legacy path. */
    sb.ctrl &= (uint8_t)~SUPERBUSTER_NBSTAB;
    check_eq("registry no decode when Z3 bus disabled",
             (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x50000000u));
    sb.ctrl |= SUPERBUSTER_NBSTAB;

    /* Reset tears the window down and the Buster stops decoding it. */
    bellatrix_zorro3_reset();
    check_eq("unmap on reset", 0u, (uint32_t)s_ext.mapped);
    check_eq("no decode after reset", (uint32_t)SUPERBUSTER_Z3_UNMAPPED,
             (uint32_t)superbuster_decode_z3(&sb, 0x40000000u));

    puts("superbuster z3 tests passed");
    return 0;
}
