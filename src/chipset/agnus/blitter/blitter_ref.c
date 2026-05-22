#include "blitter.h"

#include <string.h>

static inline uint16_t chip_read16(
    const BlitterMemory *mem,
    uint32_t addr
) {
    addr &= BLITTER_CHIP_ADDR_MASK;

    if ((addr + 1) >= mem->chipram_size) {
        return 0;
    }

    uint16_t value =
        ((uint16_t)mem->chipram[addr] << 8) |
        ((uint16_t)mem->chipram[addr + 1]);

    return value;
}

static inline void chip_write16(
    const BlitterMemory *mem,
    uint32_t addr,
    uint16_t value
) {
    addr &= BLITTER_CHIP_ADDR_MASK;

    if ((addr + 1) >= mem->chipram_size) {
        return;
    }

    mem->chipram[addr]     = (uint8_t)(value >> 8);
    mem->chipram[addr + 1] = (uint8_t)(value & 0xFFu);
}

static uint16_t blitter_logic(
    uint8_t minterm,
    uint16_t A,
    uint16_t B,
    uint16_t C
) {
    uint16_t na = (uint16_t)~A;
    uint16_t nb = (uint16_t)~B;
    uint16_t nc = (uint16_t)~C;

    uint16_t D = 0;

    if (minterm & 0x80u) D |=  (A &  B &  C);
    if (minterm & 0x40u) D |=  (A &  B & nc);
    if (minterm & 0x20u) D |=  (A & nb &  C);
    if (minterm & 0x10u) D |=  (A & nb & nc);

    if (minterm & 0x08u) D |= (na &  B &  C);
    if (minterm & 0x04u) D |= (na &  B & nc);
    if (minterm & 0x02u) D |= (na & nb &  C);
    if (minterm & 0x01u) D |= (na & nb & nc);

    return D;
}

static uint16_t shift_ascending(
    uint16_t previous,
    uint16_t current,
    uint8_t shift
) {
    if (shift == 0) {
        return current;
    }

    return (uint16_t)(
        (previous << (16 - shift)) |
        (current >> shift)
    );
}

static uint16_t shift_descending(
    uint16_t previous,
    uint16_t current,
    uint8_t shift
) {
    if (shift == 0) {
        return current;
    }

    return (uint16_t)(
        (previous >> (16 - shift)) |
        (current << shift)
    );
}

static uint16_t blitter_fill_word(
    uint16_t d,
    uint8_t *carry,
    bool inclusive,
    bool descending
) {
    uint16_t result = 0;
    uint8_t c = *carry;

    if (!descending) {
        for (int i = 15; i >= 0; --i) {
            uint8_t bit = (uint8_t)((d >> i) & 1u);

            if (inclusive) {
                if (bit)
                    c ^= 1u;
                if (c)
                    result |= (uint16_t)(1u << i);
            } else {
                if ((c ^ bit) != 0)
                    result |= (uint16_t)(1u << i);
                if (bit)
                    c ^= 1u;
            }
        }
    } else {
        for (int i = 0; i <= 15; ++i) {
            uint8_t bit = (uint8_t)((d >> i) & 1u);

            if (inclusive) {
                if (bit)
                    c ^= 1u;
                if (c)
                    result |= (uint16_t)(1u << i);
            } else {
                if ((c ^ bit) != 0)
                    result |= (uint16_t)(1u << i);
                if (bit)
                    c ^= 1u;
            }
        }
    }

    *carry = c;
    return result;
}

static uint16_t blitter_apply_shift(
    bool descending,
    uint16_t previous,
    uint16_t current,
    uint8_t shift
) {
    if (descending) {
        return shift_descending(previous, current, shift);
    }

    return shift_ascending(previous, current, shift);
}

static uint16_t blitter_apply_a_masks(
    const BlitCommand *cmd,
    uint16_t value,
    uint32_t word_index
) {
    if (word_index == 0) {
        value &= cmd->afwm;
    }

    if (word_index + 1u == (uint32_t)cmd->width_words) {
        value &= cmd->alwm;
    }

    return value;
}

static uint16_t blitter_apply_fill(
    const BlitCommand *cmd,
    uint16_t value,
    uint8_t *fill_carry
) {
    switch (cmd->fill_mode) {
        case BLITTER_FILL_INCLUSIVE:
            return blitter_fill_word(value, fill_carry, true, cmd->descending);

        case BLITTER_FILL_EXCLUSIVE:
            return blitter_fill_word(value, fill_carry, false, cmd->descending);

        case BLITTER_FILL_NONE:
        default:
            return value;
    }
}

static uint16_t blitter_fetch_word(
    const BlitterMemory *mem,
    bool enabled,
    uint32_t *ptr,
    int increment,
    uint16_t latched,
    uint16_t *last_observed
) {
    uint16_t value = latched;

    if (!enabled) {
        return value;
    }

    value = chip_read16(mem, *ptr);
    *ptr += increment;
    *last_observed = value;

    return value;
}

typedef struct BlitterCopyState {
    uint32_t apt;
    uint32_t bpt;
    uint32_t cpt;
    uint32_t dpt;
    uint16_t previous_a;
    uint16_t previous_b;
    uint16_t last_adat;
    uint16_t last_bdat;
    uint16_t last_cdat;
    uint16_t last_ddat;
    uint8_t fill_carry;
    bool zero;
} BlitterCopyState;

static void blitter_copy_state_init(
    BlitterCopyState *state,
    const BlitCommand *cmd
) {
    memset(state, 0, sizeof(*state));

    state->apt = cmd->apt;
    state->bpt = cmd->bpt;
    state->cpt = cmd->cpt;
    state->dpt = cmd->dpt;
    state->last_adat = cmd->adat;
    state->last_bdat = cmd->bdat;
    state->last_cdat = cmd->cdat;
    state->fill_carry = cmd->fill_carry_in ? 1u : 0u;
    state->zero = true;
}

static void blitter_copy_advance_line(
    BlitterCopyState *state,
    const BlitCommand *cmd
) {
    if (cmd->descending) {
        state->apt -= cmd->amod;
        state->bpt -= cmd->bmod;
        state->cpt -= cmd->cmod;
        state->dpt -= cmd->dmod;
        return;
    }

    state->apt += cmd->amod;
    state->bpt += cmd->bmod;
    state->cpt += cmd->cmod;
    state->dpt += cmd->dmod;
}

static void blitter_copy_publish_result(
    const BlitterCopyState *state,
    BlitterResult *result
) {
    result->final_apt = state->apt;
    result->final_bpt = state->bpt;
    result->final_cpt = state->cpt;
    result->final_dpt = state->dpt;

    result->final_adat = state->last_adat;
    result->final_bdat = state->last_bdat;
    result->final_cdat = state->last_cdat;
    result->final_ddat = state->last_ddat;
    result->zero = state->zero;
}

static bool execute_line_mode(
    BlitterState *b,
    BlitterMemory mem
) {
    const BlitCommand *cmd = &b->cmd;
    BlitterResult *result = &b->result;
    int16_t error = (int16_t)(cmd->apt & 0xFFFFu);
    int start_pixel = (int)cmd->line_start_bit;
    int pattern_shift = (int)cmd->bshift;
    uint16_t pattern = cmd->bdat;
    uint32_t plane_addr = cmd->cpt & BLITTER_CHIP_ADDR_MASK;
    uint32_t last_addr = plane_addr;
    int plane_mod = (int)cmd->cmod;
    int d = 0;
    bool zero = true;
    uint16_t last_cdat = cmd->cdat;

    if (cmd->height_lines == 0) {
        result->final_apt = cmd->apt;
        result->final_bpt = cmd->bpt;
        result->final_cpt = cmd->cpt;
        result->final_dpt = cmd->dpt;
        result->final_adat = cmd->adat;
        result->final_bdat = cmd->bdat;
        result->final_cdat = cmd->cdat;
        result->final_ddat = 0;
        result->zero = true;
        return true;
    }

    if (pattern_shift != 0) {
        pattern = (uint16_t)((pattern >> pattern_shift) |
                             (pattern << (16 - pattern_shift)));
    }

    for (uint16_t i = 0; i < cmd->height_lines; ++i) {
        int offset;
        uint32_t addr = plane_addr;
        uint16_t pixel;
        uint16_t bitmask;
        uint16_t dval;

        switch (cmd->line_octant)
        {
        case 0:
            offset = d + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) + (uint32_t)(i * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 1:
            offset = d + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) - (uint32_t)(i * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 2:
            offset = d + (15 - start_pixel);
            addr = (plane_addr + 1u) - (uint32_t)(offset >> 3) + (uint32_t)(i * plane_mod);
            bitmask = (uint16_t)(0x0001u << (offset & 15));
            break;

        case 3:
            offset = d + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) - (uint32_t)(i * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 4:
            offset = (int)i + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) + (uint32_t)(d * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 5:
            offset = (int)i + (15 - start_pixel);
            addr = (plane_addr + 1u) - (uint32_t)(offset >> 3) + (uint32_t)(d * plane_mod);
            bitmask = (uint16_t)(0x0001u << (offset & 15));
            break;

        case 6:
            offset = (int)i + start_pixel;
            addr = plane_addr + (uint32_t)(offset >> 3) - (uint32_t)(d * plane_mod);
            bitmask = (uint16_t)(0x8000u >> (offset & 15));
            break;

        case 7:
        default:
            offset = (int)i + (15 - start_pixel);
            addr = (plane_addr + 1u) - (uint32_t)(offset >> 3) - (uint32_t)(d * plane_mod);
            bitmask = (uint16_t)(0x0001u << (offset & 15));
            break;
        }

        pixel = chip_read16(&mem, addr);
        last_cdat = pixel;
        dval = blitter_logic(cmd->minterm, bitmask, pattern, pixel);
        chip_write16(&mem, addr, dval);

        if (dval != 0) {
            zero = false;
        }

        result->final_ddat = dval;
        last_addr = addr;

        if (error > 0) {
            error = (int16_t)(error + cmd->amod);

            if (cmd->line_octant == 3)
                d -= 1;
            else
                d += 1;
        } else {
            error = (int16_t)(error + cmd->bmod);
        }
    }

    result->final_apt = (cmd->apt & 0xFFFF0000u) | (uint16_t)error;
    result->final_bpt = cmd->bpt;
    result->final_cpt = last_addr;
    result->final_dpt = last_addr;

    result->final_adat = cmd->adat;
    result->final_bdat = cmd->bdat;
    result->final_cdat = last_cdat;
    result->zero = zero;

    return true;
}

static bool execute_copy_mode(
    BlitterState *b,
    BlitterMemory mem
) {
    const BlitCommand *cmd = &b->cmd;
    BlitterResult *result = &b->result;
    BlitterCopyState state;
    int increment = cmd->descending ? -2 : 2;

    /*
     * TODO: Revisit copy-path semantics as a first-class model:
     * define the canonical boundary between raw fetch, hold/continuity,
     * shift alignment, A masks, latched channel values, fill, and final
     * observable register publication.
     */
    blitter_copy_state_init(&state, cmd);

    for (uint32_t y = 0; y < cmd->height_lines; ++y) {
        state.previous_a = 0;
        state.previous_b = 0;
        state.fill_carry = cmd->fill_carry_in ? 1u : 0u;

        for (uint32_t x = 0; x < cmd->width_words; ++x) {
            uint16_t adat =
                blitter_fetch_word(
                    &mem,
                    cmd->use_a,
                    &state.apt,
                    increment,
                    cmd->adat,
                    &state.last_adat
                );

            uint16_t bdat =
                blitter_fetch_word(
                    &mem,
                    cmd->use_b,
                    &state.bpt,
                    increment,
                    cmd->bdat,
                    &state.last_bdat
                );

            uint16_t cdat =
                blitter_fetch_word(
                    &mem,
                    cmd->use_c,
                    &state.cpt,
                    increment,
                    cmd->cdat,
                    &state.last_cdat
                );

            uint16_t A =
                blitter_apply_shift(
                    cmd->descending,
                    state.previous_a,
                    adat,
                    cmd->ashift
                );

            uint16_t B =
                blitter_apply_shift(
                    cmd->descending,
                    state.previous_b,
                    bdat,
                    cmd->bshift
                );

            uint16_t C = cdat;
            uint16_t D;

            A = blitter_apply_a_masks(cmd, A, x);

            state.previous_a = adat;
            state.previous_b = bdat;

            D = blitter_logic(cmd->minterm, A, B, C);
            D = blitter_apply_fill(cmd, D, &state.fill_carry);

            if (D != 0) {
                state.zero = false;
            }

            if (cmd->use_d) {
                chip_write16(&mem, state.dpt, D);
                state.dpt += increment;
            }

            state.last_ddat = D;
        }

        blitter_copy_advance_line(&state, cmd);
    }

    blitter_copy_publish_result(&state, result);
    return true;
}

bool blitter_execute_reference(
    BlitterState *b,
    BlitterMemory mem
) {
    if (!b->command_valid) {
        return false;
    }

    memset(&b->result, 0, sizeof(b->result));

    switch (b->cmd.mode) {

        case BLITTER_MODE_COPY:
            return execute_copy_mode(b, mem);

        case BLITTER_MODE_LINE:
            return execute_line_mode(b, mem);

        default:
            return false;
    }
}
