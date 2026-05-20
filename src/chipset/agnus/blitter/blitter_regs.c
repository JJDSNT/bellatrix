#include "blitter.h"

#include <string.h>

enum {
    REG_BLTDDAT = 0x000,
    REG_DMACONR = 0x002,

    REG_BLTCON0 = 0x040,
    REG_BLTCON1 = 0x042,

    REG_BLTAFWM = 0x044,
    REG_BLTALWM = 0x046,

    REG_BLTAPTH = 0x050,
    REG_BLTAPTL = 0x052,

    REG_BLTBPTH = 0x04C,
    REG_BLTBPTL = 0x04E,

    REG_BLTCPTH = 0x048,
    REG_BLTCPTL = 0x04A,

    REG_BLTDPTH = 0x054,
    REG_BLTDPTL = 0x056,

    REG_BLTSIZE = 0x058,

    REG_BLTAMOD = 0x064,
    REG_BLTBMOD = 0x062,
    REG_BLTCMOD = 0x060,
    REG_BLTDMOD = 0x066,

    REG_BLTCDAT = 0x072,
    REG_BLTBDAT = 0x074,
    REG_BLTADAT = 0x076
};

static inline uint32_t make_ptr(
    uint32_t old_value,
    uint16_t value,
    bool high
) {
    if (high) {
        return ((uint32_t)value << 16) | (old_value & 0x0000FFFFu);
    }

    return (old_value & 0xFFFF0000u) | (uint32_t)(value & 0xFFFEu);
}

void blitter_init(BlitterState *b)
{
    memset(b, 0, sizeof(*b));

    b->regs.bltafwm = 0xFFFFu;
    b->regs.bltalwm = 0xFFFFu;
    b->result.zero = true;

    b->exec_state = BLITTER_EXEC_IDLE;
}

void blitter_reset(BlitterState *b)
{
    blitter_init(b);
}

int blitter_is_busy(const BlitterState *b)
{
    return b->busy ? 1 : 0;
}

uint16_t blitter_read_reg16(
    const BlitterState *b,
    uint32_t custom_offset
) {
    switch (custom_offset) {

        case REG_BLTCON0:
            return b->regs.bltcon0;

        case REG_BLTCON1:
            return b->regs.bltcon1;

        case REG_BLTAFWM:
            return b->regs.bltafwm;

        case REG_BLTALWM:
            return b->regs.bltalwm;

        case REG_BLTAMOD:
            return (uint16_t)b->regs.bltamod;

        case REG_BLTBMOD:
            return (uint16_t)b->regs.bltbmod;

        case REG_BLTCMOD:
            return (uint16_t)b->regs.bltcmod;

        case REG_BLTDMOD:
            return (uint16_t)b->regs.bltdmod;

        case REG_BLTADAT:
            return b->regs.bltadat;

        case REG_BLTBDAT:
            return b->regs.bltbdat;

        case REG_BLTCDAT:
            return b->regs.bltcdat;

        case REG_BLTDDAT:
            return b->regs.bltddat;

        case REG_BLTSIZE:
            return b->regs.bltsize;

        case REG_DMACONR:
        {
            uint16_t value = 0;

            if (b->busy) {
                value |= BLITTER_DMACON_BLTBUSY;
            }

            if (b->result.zero) {
                value |= BLITTER_DMACON_BLTNZERO;
            }

            return value;
        }

        default:
            return 0;
    }
}

void blitter_write_reg16(
    BlitterState *b,
    uint32_t custom_offset,
    uint16_t value
) {
    switch (custom_offset) {

        case REG_BLTCON0:
            b->regs.bltcon0 = value;
            return;

        case REG_BLTCON1:
            b->regs.bltcon1 = value;
            return;

        case REG_BLTAFWM:
            b->regs.bltafwm = value;
            return;

        case REG_BLTALWM:
            b->regs.bltalwm = value;
            return;

        case REG_BLTAMOD:
            b->regs.bltamod = (int16_t)value;
            return;

        case REG_BLTBMOD:
            b->regs.bltbmod = (int16_t)value;
            return;

        case REG_BLTCMOD:
            b->regs.bltcmod = (int16_t)value;
            return;

        case REG_BLTDMOD:
            b->regs.bltdmod = (int16_t)value;
            return;

        case REG_BLTADAT:
            b->regs.bltadat = value;
            return;

        case REG_BLTBDAT:
            b->regs.bltbdat = value;
            return;

        case REG_BLTCDAT:
            b->regs.bltcdat = value;
            return;

        case REG_BLTDDAT:
            b->regs.bltddat = value;
            return;

        case REG_BLTAPTH:
            b->regs.bltapt =
                make_ptr(b->regs.bltapt, value, true);
            return;

        case REG_BLTAPTL:
            b->regs.bltapt =
                make_ptr(b->regs.bltapt, value, false);
            return;

        case REG_BLTBPTH:
            b->regs.bltbpt =
                make_ptr(b->regs.bltbpt, value, true);
            return;

        case REG_BLTBPTL:
            b->regs.bltbpt =
                make_ptr(b->regs.bltbpt, value, false);
            return;

        case REG_BLTCPTH:
            b->regs.bltcpt =
                make_ptr(b->regs.bltcpt, value, true);
            return;

        case REG_BLTCPTL:
            b->regs.bltcpt =
                make_ptr(b->regs.bltcpt, value, false);
            return;

        case REG_BLTDPTH:
            b->regs.bltdpt =
                make_ptr(b->regs.bltdpt, value, true);
            return;

        case REG_BLTDPTL:
            b->regs.bltdpt =
                make_ptr(b->regs.bltdpt, value, false);
            return;

        case REG_BLTSIZE:
        {
            b->regs.bltsize = value;

            b->regs.bltsizh = value & 0x003Fu;
            b->regs.bltsizv = (value >> 6) & 0x03FFu;

            if (b->regs.bltsizh == 0) {
                b->regs.bltsizh = 64;
            }

            if (b->regs.bltsizv == 0) {
                b->regs.bltsizv = 1024;
            }

            blitter_begin_command(b);

            return;
        }

        default:
            return;
    }
}
