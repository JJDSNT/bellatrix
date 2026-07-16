/*
 * Bellatrix platform adapter for Emu68's native fault path.
 *
 * This file is intentionally included by vectors.c instead of compiled as a
 * separate translation unit. The Emu68 SYSPageFault handlers therefore keep
 * their original direct call shape: no function-pointer dispatch, queue or
 * extra adapter call is introduced on the MMIO path. bellatrix_bus_access is
 * transitional and must be decomposed into sparse region-owner routes rather
 * than promoted to a closed machine ABI.
 */

#include "cpu/emu68/bellatrix.h"
#include "cpu/cpu_bridge.h"

#if BELLATRIX_ENABLE_EMU68_BOARDS
#include <boards.h>

static int bellatrix_board_idx;
static struct ExpansionBoard **bellatrix_board;

extern struct ExpansionBoard *__boards_start;
extern int zorro_disable;

static void bellatrix_emu68_boards_init_once(void)
{
    if (!bellatrix_board)
        bellatrix_board = &__boards_start;
}

void bellatrix_emu68_boards_reset(void)
{
    bellatrix_board = &__boards_start;
    bellatrix_board_idx = 0;
}

static struct ExpansionBoard *bellatrix_emu68_current_board(void)
{
    bellatrix_emu68_boards_init_once();

    while (bellatrix_board[bellatrix_board_idx] &&
           !bellatrix_board[bellatrix_board_idx]->enabled) {
        bellatrix_board_idx++;
    }

    return bellatrix_board[bellatrix_board_idx];
}

static int bellatrix_emu68_handle_board_write(uint32_t far, uint32_t value)
{
    struct ExpansionBoard *board;

    if (zorro_disable || far < 0x00e80000u || far > 0x00e8ffffu)
        return 0;

    board = bellatrix_emu68_current_board();
    if (!board)
        return 0;

    if (board->is_z3) {
        if (far == 0x00e80044u) {
            board->map_base = (value & 0xffffu) << 16;
            board->map(board);
            bellatrix_board_idx++;
            return 1;
        }
    } else if (far == 0x00e80048u) {
        board->map_base = (value & 0xffu) << 16;
        board->map(board);
        bellatrix_board_idx++;
        return 1;
    }

    if (far == 0x00e8004cu || far == 0x00e8004eu) {
        bellatrix_board_idx++;
        return 1;
    }

    return 0;
}

static int bellatrix_emu68_handle_board_read(uint64_t *value, int size,
                                               uint32_t far)
{
    struct ExpansionBoard *board;

    if (zorro_disable || size != 1 ||
        far < 0x00e80000u || far > 0x00e8ffffu)
        return 0;

    board = bellatrix_emu68_current_board();
    if (!board)
        return 0;

    *value = board->rom_file[far - 0x00e80000u];
    return 1;
}
#else
void bellatrix_emu68_boards_reset(void)
{
}

static int bellatrix_emu68_handle_board_write(uint32_t far, uint32_t value)
{
    (void)far;
    (void)value;
    return 0;
}

static int bellatrix_emu68_handle_board_read(uint64_t *value, int size,
                                               uint32_t far)
{
    (void)value;
    (void)size;
    (void)far;
    return 0;
}
#endif

int SYSWriteValToAddr(uint64_t value, uint64_t value2, int size, uint64_t far)
{
    uint64_t x18_save;
    uint64_t v30_save;

    D(kprintf("[JIT:SYS] SYSWriteValToAddr(0x%x, %d, %p)\n",
              value, size, far));
    (void)value2;

    if ((far >> 32) == 1 || (far >> 32) == 0xffffffff)
        far &= 0xffffffff;

    /* The bridge may call kprintf, which clobbers the pinned M68K PC in x18
     * and the modeled-cycle counter in v30. Preserve both across the call. */
    asm volatile("mov %0, x18" : "=r"(x18_save));
    asm volatile("mov %0, v30.d[0]" : "=r"(v30_save));
    bellatrix_emu68_report_jit_progress(v30_save, (uint32_t)x18_save);

    if (!bellatrix_emu68_handle_board_write((uint32_t)far, (uint32_t)value)) {
        g_bellatrix_fault_pc = (uint32_t)x18_save;
        bellatrix_bus_access((uint32_t)far, (uint32_t)value, size, BUS_WRITE);
    }

    asm volatile("mov v30.d[0], %0" :: "r"(v30_save));
    asm volatile("mov x18, %0" :: "r"(x18_save));
    return 1;
}

int SYSReadValFromAddr(uint64_t *value, uint64_t *value2, int size,
                       uint64_t far)
{
    uint64_t x18_save;
    uint64_t v30_save;

    D(kprintf("[JIT:SYS] SYSReadValFromAddr(%d, %p)\n", size, far));

    if ((far >> 32) == 1 || (far >> 32) == 0xffffffff)
        far &= 0xffffffff;

    asm volatile("mov %0, x18" : "=r"(x18_save));
    asm volatile("mov %0, v30.d[0]" : "=r"(v30_save));
    bellatrix_emu68_report_jit_progress(v30_save, (uint32_t)x18_save);

    if (!bellatrix_emu68_handle_board_read(value, size, (uint32_t)far)) {
        g_bellatrix_fault_pc = (uint32_t)x18_save;
        *value = bellatrix_bus_access((uint32_t)far, 0, size, BUS_READ);
    }

    asm volatile("mov v30.d[0], %0" :: "r"(v30_save));
    asm volatile("mov x18, %0" :: "r"(x18_save));
    if (value2)
        *value2 = 0;
    return 1;
}
