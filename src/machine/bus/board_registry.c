#include "machine/bus/board_registry.h"

#include "machine/autoconfig/autoconfig.h"
#include "machine/memory/memory.h"

/*
 * Boundary symbols the linker synthesises for the C-identifier section named by
 * BELLATRIX_BOARDS_SECTION. Declared weak so a link with no registered board
 * resolves both to NULL (empty table) instead of failing to link.
 */
extern struct ExpansionBoard *const __start_bellatrix_boards[] __attribute__((weak));
extern struct ExpansionBoard *const __stop_bellatrix_boards[] __attribute__((weak));

size_t bellatrix_board_count(void)
{
    if (!__start_bellatrix_boards || !__stop_bellatrix_boards)
        return 0u;
    return (size_t)(__stop_bellatrix_boards - __start_bellatrix_boards);
}

struct ExpansionBoard *bellatrix_board_at(size_t index)
{
    if (index >= bellatrix_board_count())
        return NULL;
    return __start_bellatrix_boards[index];
}

/* Autoconfig walk position: index of the board currently being configured. */
static size_t s_board_idx;

/* Advance past disabled boards and return the board now under configuration,
 * or NULL when the table is exhausted (matches Emu68's read-side skip). */
static struct ExpansionBoard *current_board(void)
{
    struct ExpansionBoard *board;
    while ((board = bellatrix_board_at(s_board_idx)) != NULL && !board->enabled)
        s_board_idx++;
    return board;
}

void bellatrix_boards_autoconfig_reset(void)
{
    s_board_idx = 0u;
}

uint8_t bellatrix_boards_autoconfig_read8(uint32_t addr)
{
    struct ExpansionBoard *board = current_board();
    uint32_t off;

    if (!board || !board->rom_file)
        return 0xffu;
    off = addr - BELLATRIX_Z2_CONFIG_BASE;
    if (off >= AUTOCONFIG_DATA_SIZE)
        return 0xffu; /* only the nibble-encoded image answers; rest is quiet */
    return ((const uint8_t *)board->rom_file)[off];
}

void bellatrix_boards_autoconfig_write(uint32_t addr, uint32_t value,
                                       unsigned int size)
{
    struct ExpansionBoard *board = current_board();
    uint32_t off;

    (void)size;
    if (!board)
        return;
    off = (addr - BELLATRIX_Z2_CONFIG_BASE) & 0xffffu;

    if (board->is_z3) {
        if (off == AC_OFF_Z3_HI) {
            board->map_base = (value & 0xffffu) << 16;
            if (board->map)
                board->map(board);
            s_board_idx++;
            return;
        }
    } else {
        if (off == AC_OFF_BASE_HI) {
            board->map_base = (value & 0xffu) << 16;
            if (board->map)
                board->map(board);
            s_board_idx++;
            return;
        }
    }

    if (off == AC_OFF_SHUTUP || off == (AC_OFF_SHUTUP + 2u)) {
        /* Board declined its base; present the next one. */
        board->map_base = 0u;
        s_board_idx++;
    }
}
