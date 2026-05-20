#ifndef BELLATRIX_AGNUS_BLITTER_H
#define BELLATRIX_AGNUS_BLITTER_H

#include <stdbool.h>
#include <stdint.h>

#include "blitter_types.h"

enum
{
    AGNUS_BLTCON0 = 0x040,
    AGNUS_BLTCON1 = 0x042,
    AGNUS_BLTAFWM = 0x044,
    AGNUS_BLTALWM = 0x046,

    AGNUS_BLTCPTH = 0x048,
    AGNUS_BLTCPTL = 0x04A,
    AGNUS_BLTBPTH = 0x04C,
    AGNUS_BLTBPTL = 0x04E,
    AGNUS_BLTAPTH = 0x050,
    AGNUS_BLTAPTL = 0x052,
    AGNUS_BLTDPTH = 0x054,
    AGNUS_BLTDPTL = 0x056,

    AGNUS_BLTSIZE = 0x058,

    AGNUS_BLTCMOD = 0x060,
    AGNUS_BLTBMOD = 0x062,
    AGNUS_BLTAMOD = 0x064,
    AGNUS_BLTDMOD = 0x066,

    AGNUS_BLTCDAT = 0x070,
    AGNUS_BLTBDAT = 0x072,
    AGNUS_BLTADAT = 0x074
};

enum
{
    AGNUS_DMACON_BBUSY = 0x4000,
    AGNUS_DMACON_BZERO = 0x2000
};

typedef struct BlitterState
{
    BlitterRegs regs;
    BlitCommand cmd;
    BlitterResult result;
    BlitterExecState exec_state;

    bool command_valid;
    bool busy;

    uint32_t cycles_remaining;
    uint32_t dma_slots_consumed;

    bool debug_trace;
} BlitterState;

#ifdef __cplusplus
extern "C" {
#endif

void blitter_init(BlitterState *b);
void blitter_reset(BlitterState *b);

uint16_t blitter_read_reg16(const BlitterState *b, uint32_t custom_offset);

void blitter_write_reg16(
    BlitterState *b,
    uint32_t custom_offset,
    uint16_t value
);

int blitter_is_busy(const BlitterState *b);

void blitter_build_command(BlitterState *b);
void blitter_begin_command(BlitterState *b);
void blitter_publish_result(BlitterState *b);

bool blitter_execute_reference(
    BlitterState *b,
    BlitterMemory mem
);

uint32_t blitter_estimate_cycles(const BlitCommand *cmd);

void blitter_start_timing(BlitterState *b);

void blitter_step_dma(
    BlitterState *b,
    BlitterMemory mem,
    BlitterHost host,
    uint32_t dma_slots
);

void blitter_force_finish(
    BlitterState *b,
    BlitterHost host
);

void blitter_debug_dump_regs(const BlitterState *b);
void blitter_debug_dump_command(const BlitterState *b);
void blitter_debug_set_trace(BlitterState *b, bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_AGNUS_BLITTER_H */
