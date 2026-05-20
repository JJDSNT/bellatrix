#include "blitter.h"

#include <string.h>

static BlitterMode decode_mode(uint16_t bltcon1)
{
    if (bltcon1 & 0x0001u) {
        return BLITTER_MODE_LINE;
    }

    return BLITTER_MODE_COPY;
}

static BlitterFillMode decode_fill_mode(uint16_t bltcon1)
{
    switch ((bltcon1 >> 3) & 0x03u) {
        case 1:
            return BLITTER_FILL_INCLUSIVE;

        case 2:
            return BLITTER_FILL_EXCLUSIVE;

        default:
            return BLITTER_FILL_NONE;
    }
}

void blitter_build_command(BlitterState *b)
{
    BlitCommand *cmd = &b->cmd;
    const BlitterRegs *r = &b->regs;

    memset(cmd, 0, sizeof(*cmd));

    cmd->apt = r->bltapt & BLITTER_CHIP_ADDR_MASK;
    cmd->bpt = r->bltbpt & BLITTER_CHIP_ADDR_MASK;
    cmd->cpt = r->bltcpt & BLITTER_CHIP_ADDR_MASK;
    cmd->dpt = r->bltdpt & BLITTER_CHIP_ADDR_MASK;

    cmd->amod = r->bltamod;
    cmd->bmod = r->bltbmod;
    cmd->cmod = r->bltcmod;
    cmd->dmod = r->bltdmod;

    cmd->adat = r->bltadat;
    cmd->bdat = r->bltbdat;
    cmd->cdat = r->bltcdat;

    cmd->afwm = r->bltafwm;
    cmd->alwm = r->bltalwm;

    cmd->width_words = r->bltsizh;
    cmd->height_lines = r->bltsizv;

    cmd->use_a = (r->bltcon0 & (BLITTER_CHANNEL_A << 8)) != 0;
    cmd->use_b = (r->bltcon0 & (BLITTER_CHANNEL_B << 8)) != 0;
    cmd->use_c = (r->bltcon0 & (BLITTER_CHANNEL_C << 8)) != 0;
    cmd->use_d = (r->bltcon0 & (BLITTER_CHANNEL_D << 8)) != 0;

    cmd->minterm = (uint8_t)(r->bltcon0 & 0x00FFu);

    cmd->ashift = (uint8_t)((r->bltcon0 >> 12) & 0x0Fu);
    cmd->bshift = (uint8_t)((r->bltcon1 >> 12) & 0x0Fu);

    cmd->descending = (r->bltcon1 & 0x0002u) != 0;
    cmd->fill_mode = decode_fill_mode(r->bltcon1);
    cmd->fill_carry_in =
        (r->bltcon1 & 0x0004u) != 0;

    cmd->line_octant =
        (uint8_t)((r->bltcon1 >> 2) & 0x07u);

    cmd->line_start_bit =
        (uint8_t)((r->bltcon0 >> 12) & 0x0Fu);

    cmd->mode = decode_mode(r->bltcon1);

    b->command_valid = true;
}
