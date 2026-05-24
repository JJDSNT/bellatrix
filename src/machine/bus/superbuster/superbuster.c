#include "machine/bus/superbuster/superbuster.h"

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
