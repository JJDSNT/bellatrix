#include "machine/bus/superbuster/superbuster.h"

#include "machine/bus/board_registry.h"

void superbuster_init(SuperBusterState *s)
{
    /* NBSTAB=1: report Z3 bus available; high nibble = revision 0xF */
    s->ctrl = SUPERBUSTER_NBSTAB | SUPERBUSTER_BUSTER_ID;
}

void superbuster_reset(SuperBusterState *s)
{
    superbuster_init(s);
}

int superbuster_owns(uint32_t addr)
{
    return (addr >= SUPERBUSTER_BASE && addr <= SUPERBUSTER_END);
}

uint8_t superbuster_read8(SuperBusterState *s, uint32_t addr)
{
    uint32_t off = addr - SUPERBUSTER_BASE;
    if (off == 0)
        return s->ctrl;
    return 0xFFu;
}

void superbuster_write8(SuperBusterState *s, uint32_t addr, uint8_t value)
{
    uint32_t off = addr - SUPERBUSTER_BASE;
    if (off == 0)
        s->ctrl = value;
}

SuperBusterZ3Decode superbuster_decode_z3(const SuperBusterState *s,
                                          uint32_t addr)
{
    /* The Z3 bus must be reported available (NBSTAB) for the Buster to answer
     * for any slot. This is the chip's own state, not an address property. */
    if (!s || (s->ctrl & SUPERBUSTER_NBSTAB) == 0u)
        return SUPERBUSTER_Z3_UNMAPPED;
    /* A configured EXTERNAL Z3 board (board_registry) owns this window. */
    {
        const struct ExpansionBoard *b =
            bellatrix_boards_external_window_owner(addr);
        if (b && b->is_z3)
            return SUPERBUSTER_Z3_BOARD;
    }
    return SUPERBUSTER_Z3_UNMAPPED;
}
