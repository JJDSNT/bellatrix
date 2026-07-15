#include "machine/bus/zorro3/zorro3.h"

#include "machine/autoconfig/autoconfig.h"
#include "support.h"

#include <string.h>

#ifndef BELLATRIX_MAX_ZORRO3_BOARDS
#define BELLATRIX_MAX_ZORRO3_BOARDS 8
#endif

/*
 * Zorro 3 shares the $E80000 config window with Z2.  The actual config-window
 * reads/writes for Z3 boards go through bellatrix_zorro3_config_* which the
 * shared autoconfig sequencer (zorro2_bus.c) calls once it detects er_Type
 * bits 7-6 = 10.
 *
 * For now the Z3 state machine is standalone: it tracks its own current board
 * pointer and expects the machine to pick either Z2 or Z3 routing based on
 * the er_Type of the current board.
 */

typedef struct Zorro3Board {
    BellatrixZorro3BoardDesc desc;
    uint8_t  enabled;
    uint8_t  configured;
    uint8_t  shutup;
    uint32_t base;
    uint8_t  hi_byte;   /* latched from AC_OFF_Z3_HI write */
} Zorro3Board;

static Zorro3Board s_boards[BELLATRIX_MAX_ZORRO3_BOARDS];
static size_t      s_count;
static int         s_current;

static int find_slot(const char *id)
{
    size_t i;
    if (!id) return -1;
    for (i = 0; i < s_count; ++i)
        if (s_boards[i].desc.id && strcmp(s_boards[i].desc.id, id) == 0)
            return (int)i;
    return -1;
}

static int first_pending(void)
{
    size_t i;
    for (i = 0; i < s_count; ++i)
        if (s_boards[i].enabled && !s_boards[i].configured && !s_boards[i].shutup)
            return (int)i;
    return -1;
}

static void refresh_current(void)
{
    s_current = first_pending();
}

static Zorro3Board *current_board(void)
{
    if (s_current < 0 || (size_t)s_current >= s_count)
        return NULL;
    return &s_boards[s_current];
}

static Zorro3Board *board_for_addr(uint32_t addr)
{
    size_t i;
    for (i = 0; i < s_count; ++i) {
        Zorro3Board *b = &s_boards[i];
        if (!b->enabled || !b->configured || !b->base || !b->desc.window_size)
            continue;
        if (addr >= b->base && addr < b->base + b->desc.window_size)
            return b;
    }
    return NULL;
}

void bellatrix_zorro3_init(void)
{
    size_t i;
    for (i = 0; i < s_count; ++i) {
        s_boards[i].configured = 0;
        s_boards[i].shutup     = 0;
        s_boards[i].base       = 0;
        s_boards[i].hi_byte    = 0;
    }
    refresh_current();
}

void bellatrix_zorro3_reset(void)
{
    size_t i;
    bellatrix_zorro3_init();
    for (i = 0; i < s_count; ++i) {
        Zorro3Board *b = &s_boards[i];
        if (b->enabled && b->desc.ops && b->desc.ops->reset)
            b->desc.ops->reset(b->desc.userdata);
    }
}

int bellatrix_zorro3_register_board(const BellatrixZorro3BoardDesc *desc)
{
    Zorro3Board *b;
    int slot;

    if (!desc || !desc->id || !desc->config_data || desc->config_size == 0)
        return -1;

    slot = find_slot(desc->id);
    if (slot >= 0) {
        b = &s_boards[slot];
        b->desc       = *desc;
        b->enabled    = 1;
        b->configured = 0;
        b->shutup     = 0;
        b->base       = 0;
        b->hi_byte    = 0;
        refresh_current();
        return 0;
    }

    if (s_count >= BELLATRIX_MAX_ZORRO3_BOARDS)
        return -2;

    b = &s_boards[s_count++];
    memset(b, 0, sizeof(*b));
    b->desc    = *desc;
    b->enabled = 1;

    if (s_current < 0)
        s_current = (int)(s_count - 1);

    kprintf("[Z3] board registered: %s  window=%08x\n",
            desc->id, (unsigned)desc->window_size);
    return 0;
}

int bellatrix_zorro3_unregister_board(const char *id)
{
    size_t i;
    int slot = find_slot(id);
    if (slot < 0) return -1;

    if (s_boards[slot].desc.ops && s_boards[slot].desc.ops->destroy)
        s_boards[slot].desc.ops->destroy(s_boards[slot].desc.userdata);

    for (i = (size_t)slot + 1; i < s_count; ++i)
        s_boards[i - 1] = s_boards[i];

    if (s_count > 0) {
        memset(&s_boards[s_count - 1], 0, sizeof(s_boards[0]));
        --s_count;
    }
    refresh_current();
    return 0;
}

/*
 * Config window writes for Z3 boards (WriteExpansionWord(board, 17, addr>>16)):
 *   AC_OFF_Z3_HI  (0x44): latch bits 31-24 of base address
 *   AC_OFF_BASE_HI (0x48): bits 23-16 of base address; triggers assignment
 *   AC_OFF_SHUTUP  (0x4C): shut up the board
 */
void bellatrix_zorro3_config_write8(uint32_t addr, uint8_t value)
{
    Zorro3Board *b;
    uint32_t off;

    b = current_board();
    if (!b) return;

    off = addr & 0xFFFFu;

    if (off == AC_OFF_Z3_HI) {
        b->hi_byte = value;
    } else if (off == AC_OFF_BASE_HI) {
        b->base       = ((uint32_t)b->hi_byte << 24) | ((uint32_t)value << 16);
        b->configured = 1;
        if (b->base == 0) {
            /* Kickstart shuts up boards it can't map by writing 0 */
            b->shutup = 1;
            kprintf("[Z3] board '%s' shutup (base=0)\n",
                    b->desc.id ? b->desc.id : "?");
        } else {
            kprintf("[Z3] board '%s' assigned base=%08x size=%08x\n",
                    b->desc.id ? b->desc.id : "?",
                    (unsigned)b->base,
                    (unsigned)b->desc.window_size);
        }
        refresh_current();
    }
}

int bellatrix_zorro3_in_board_window(uint32_t addr)
{
    return board_for_addr(addr) != NULL;
}

uint8_t bellatrix_zorro3_board_read8(uint32_t addr)
{
    Zorro3Board *b = board_for_addr(addr);
    uint32_t off;
    if (!b) return 0xFFu;
    off = addr - b->base;
    if (b->desc.ops && b->desc.ops->read8)
        return b->desc.ops->read8(b->desc.userdata, off);
    return 0xFFu;
}

uint16_t bellatrix_zorro3_board_read16(uint32_t addr)
{
    return (uint16_t)(((uint16_t)bellatrix_zorro3_board_read8(addr) << 8) |
                       bellatrix_zorro3_board_read8(addr + 1u));
}

uint32_t bellatrix_zorro3_board_read32(uint32_t addr)
{
    return ((uint32_t)bellatrix_zorro3_board_read8(addr)     << 24) |
           ((uint32_t)bellatrix_zorro3_board_read8(addr + 1) << 16) |
           ((uint32_t)bellatrix_zorro3_board_read8(addr + 2) <<  8) |
            (uint32_t)bellatrix_zorro3_board_read8(addr + 3);
}

void bellatrix_zorro3_board_write8(uint32_t addr, uint8_t value)
{
    Zorro3Board *b = board_for_addr(addr);
    if (!b) return;
    if (b->desc.ops && b->desc.ops->write8)
        b->desc.ops->write8(b->desc.userdata, addr - b->base, value);
}

void bellatrix_zorro3_board_write16(uint32_t addr, uint16_t value)
{
    bellatrix_zorro3_board_write8(addr,      (uint8_t)(value >> 8));
    bellatrix_zorro3_board_write8(addr + 1u, (uint8_t)(value & 0xFFu));
}

void bellatrix_zorro3_board_write32(uint32_t addr, uint32_t value)
{
    bellatrix_zorro3_board_write8(addr,      (uint8_t)(value >> 24));
    bellatrix_zorro3_board_write8(addr + 1u, (uint8_t)(value >> 16));
    bellatrix_zorro3_board_write8(addr + 2u, (uint8_t)(value >>  8));
    bellatrix_zorro3_board_write8(addr + 3u, (uint8_t)(value & 0xFFu));
}

int bellatrix_zorro3_board_configured(const char *id)
{
    int slot = find_slot(id);
    if (slot < 0) return 0;
    return s_boards[slot].configured && !s_boards[slot].shutup;
}

uint32_t bellatrix_zorro3_board_base(const char *id)
{
    int slot = find_slot(id);
    if (slot < 0) return 0;
    return s_boards[slot].base;
}
