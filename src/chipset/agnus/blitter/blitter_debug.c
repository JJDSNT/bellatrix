#include "blitter.h"

#include <stdio.h>

void blitter_debug_set_trace(
    BlitterState *b,
    bool enabled
) {
    b->debug_trace = enabled;
}

void blitter_debug_dump_regs(
    const BlitterState *b
) {
    const BlitterRegs *r = &b->regs;

    printf("\n");
    printf("========== BLITTER REGS ==========\n");

    printf("BLTCON0 : %04X\n", r->bltcon0);
    printf("BLTCON1 : %04X\n", r->bltcon1);

    printf("BLTAFWM : %04X\n", r->bltafwm);
    printf("BLTALWM : %04X\n", r->bltalwm);

    printf("BLTAPT  : %08X\n", r->bltapt);
    printf("BLTBPT  : %08X\n", r->bltbpt);
    printf("BLTCPT  : %08X\n", r->bltcpt);
    printf("BLTDPT  : %08X\n", r->bltdpt);

    printf("BLTAMOD : %d\n", r->bltamod);
    printf("BLTBMOD : %d\n", r->bltbmod);
    printf("BLTCMOD : %d\n", r->bltcmod);
    printf("BLTDMOD : %d\n", r->bltdmod);

    printf("BLTADAT : %04X\n", r->bltadat);
    printf("BLTBDAT : %04X\n", r->bltbdat);
    printf("BLTCDAT : %04X\n", r->bltcdat);
    printf("BLTDDAT : %04X\n", r->bltddat);

    printf("BLTSIZE : %04X\n", r->bltsize);
    printf("BLTSIZH : %u\n", r->bltsizh);
    printf("BLTSIZV : %u\n", r->bltsizv);

    printf("BUSY    : %d\n", b->busy);
    printf("STATE   : %d\n", b->exec_state);

    printf("==================================\n");
}

void blitter_debug_dump_command(
    const BlitterState *b
) {
    const BlitCommand *c = &b->cmd;

    printf("\n");
    printf("========= BLIT COMMAND =========\n");

    printf("MODE         : %d\n", c->mode);

    printf("USE_A        : %d\n", c->use_a);
    printf("USE_B        : %d\n", c->use_b);
    printf("USE_C        : %d\n", c->use_c);
    printf("USE_D        : %d\n", c->use_d);

    printf("MINTERM      : %02X\n", c->minterm);

    printf("ASHIFT       : %u\n", c->ashift);
    printf("BSHIFT       : %u\n", c->bshift);

    printf("DESCENDING   : %d\n", c->descending);

    printf("APT          : %08X\n", c->apt);
    printf("BPT          : %08X\n", c->bpt);
    printf("CPT          : %08X\n", c->cpt);
    printf("DPT          : %08X\n", c->dpt);

    printf("AMOD         : %d\n", c->amod);
    printf("BMOD         : %d\n", c->bmod);
    printf("CMOD         : %d\n", c->cmod);
    printf("DMOD         : %d\n", c->dmod);

    printf("AFWM         : %04X\n", c->afwm);
    printf("ALWM         : %04X\n", c->alwm);

    printf("WIDTH_WORDS  : %u\n", c->width_words);
    printf("HEIGHT_LINES : %u\n", c->height_lines);

    printf("LINE_OCTANT  : %u\n", c->line_octant);

    printf("FILL_MODE    : %d\n", c->fill_mode);

    printf("================================\n");
}
