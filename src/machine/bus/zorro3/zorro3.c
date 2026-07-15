#include "machine/bus/zorro3/zorro3.h"

#include "machine/autoconfig/autoconfig.h"
#include "support.h"

#include <string.h>

#ifndef BELLATRIX_MAX_ZORRO3_BOARDS
#define BELLATRIX_MAX_ZORRO3_BOARDS 8
#endif

/*
 * Zorro 3 shares the $E80000 config window with Z2. The sparse Autoconfig
 * owner in zorro_autoconfig.c presents pending Z2 boards first, followed by
 * pending Z3 boards, while each bus keeps its own registration state.
 */

typedef struct Zorro3Board {
    BellatrixZorro3BoardDesc desc;
    uint8_t  enabled;
    uint8_t  configured;
    uint8_t  shutup;
    uint32_t base;
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
        if (addr >= b->base && addr - b->base < b->desc.window_size)
            return b;
    }
    return NULL;
}

void bellatrix_zorro3_init(void)
{
    size_t i;
    for (i = 0; i < s_count; ++i) {
        if (s_boards[i].configured && !s_boards[i].shutup &&
            s_boards[i].desc.ops && s_boards[i].desc.ops->unmap) {
            s_boards[i].desc.ops->unmap(s_boards[i].desc.userdata,
                                        s_boards[i].base,
                                        s_boards[i].desc.window_size);
        }
        s_boards[i].configured = 0;
        s_boards[i].shutup     = 0;
        s_boards[i].base       = 0;
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
        if (b->configured && !b->shutup && b->desc.ops && b->desc.ops->unmap)
            b->desc.ops->unmap(b->desc.userdata, b->base,
                               b->desc.window_size);
        b->desc       = *desc;
        b->enabled    = 1;
        b->configured = 0;
        b->shutup     = 0;
        b->base       = 0;
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

    if (s_boards[slot].configured && !s_boards[slot].shutup &&
        s_boards[slot].desc.ops && s_boards[slot].desc.ops->unmap) {
        s_boards[slot].desc.ops->unmap(s_boards[slot].desc.userdata,
                                       s_boards[slot].base,
                                       s_boards[slot].desc.window_size);
    }
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
 * Emu68's contract for a Z3 board is WriteExpansionWord(board, 17,
 * base >> 16): a 16-bit value at offset 0x44 completes assignment and map().
 * Offset 0x4c shuts the board up.
 */
int bellatrix_zorro3_has_pending_board(void)
{
    return current_board() != NULL;
}

uint8_t bellatrix_zorro3_config_read8(uint32_t addr)
{
    Zorro3Board *b = current_board();
    uint32_t off;

    if (!b)
        return 0xffu;
    off = addr & 0xffffu;
    if (off >= b->desc.config_size)
        return 0xffu;
    return b->desc.config_data[off];
}

uint16_t bellatrix_zorro3_config_read16(uint32_t addr)
{
    return (uint16_t)(((uint16_t)bellatrix_zorro3_config_read8(addr) << 8) |
                      bellatrix_zorro3_config_read8(addr + 1u));
}

uint32_t bellatrix_zorro3_config_read32(uint32_t addr)
{
    return ((uint32_t)bellatrix_zorro3_config_read8(addr) << 24) |
           ((uint32_t)bellatrix_zorro3_config_read8(addr + 1u) << 16) |
           ((uint32_t)bellatrix_zorro3_config_read8(addr + 2u) << 8) |
           (uint32_t)bellatrix_zorro3_config_read8(addr + 3u);
}

static void configure_current_board(uint32_t base)
{
    Zorro3Board *b = current_board();
    int rc = 0;

    if (!b)
        return;
    if (base == 0u) {
        b->base = 0u;
        b->configured = 1;
        b->shutup = 1;
        kprintf("[Z3] board '%s' shutup (base=0)\n",
                b->desc.id ? b->desc.id : "?");
    } else {
        if (b->desc.ops && b->desc.ops->map)
            rc = b->desc.ops->map(b->desc.userdata, base,
                                  b->desc.window_size);
        if (rc != 0) {
            kprintf("[Z3] board '%s' map failed rc=%d base=%08x size=%08x\n",
                    b->desc.id ? b->desc.id : "?", rc, (unsigned)base,
                    (unsigned)b->desc.window_size);
            b->base = 0u;
            b->configured = 0;
            return;
        }
        b->base = base;
        b->configured = 1;
        kprintf("[Z3] board '%s' assigned base=%08x size=%08x\n",
                b->desc.id ? b->desc.id : "?", (unsigned)b->base,
                (unsigned)b->desc.window_size);
    }
    refresh_current();
}

void bellatrix_zorro3_config_write8(uint32_t addr, uint8_t value)
{
    uint32_t off;

    if (!current_board()) return;
    off = addr & 0xFFFFu;
    if (off == AC_OFF_Z3_HI) {
        configure_current_board((uint32_t)value << 16);
    } else if (off == AC_OFF_SHUTUP) {
        configure_current_board(0u);
    }
}

void bellatrix_zorro3_config_write16(uint32_t addr, uint16_t value)
{
    uint32_t off = addr & 0xffffu;
    if (off == AC_OFF_Z3_HI)
        configure_current_board((uint32_t)value << 16);
    else if (off == AC_OFF_SHUTUP)
        configure_current_board(0u);
}

void bellatrix_zorro3_config_write32(uint32_t addr, uint32_t value)
{
    /* Match Emu68: only the low 16 bits of the value form base[31:16]. */
    bellatrix_zorro3_config_write16(addr, (uint16_t)value);
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
