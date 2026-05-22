#include "bus/zorro2/zorro2_bus.h"

#include "support.h"

#include <string.h>

#ifndef BELLATRIX_MAX_ZORRO2_BOARDS
#define BELLATRIX_MAX_ZORRO2_BOARDS 16
#endif

#define AUTOCONFIG_Z2_BASE 0x00E80000u
#define AUTOCONFIG_Z2_MASK 0x0007FFFFu

#define AUTOCONFIG_REG_ASSIGN_HI 0x48u
#define AUTOCONFIG_REG_SHUTUP    0x4Eu

#define AUTOCONFIG_PRODUCT_ID      0x10u
#define AUTOCONFIG_MANUFACTURER_ID 0x6d73u
#define AUTOCONFIG_RAM_SERIAL      0x1e0aeb68u
#define AUTOCONFIG_CD_PRODUCT_ID   0x11u

typedef struct BellatrixZorro2Board {
    BellatrixZorro2BoardDesc desc;
    uint8_t enabled;
    uint8_t configured;
    uint8_t shutup;
    uint32_t assigned_base;
} BellatrixZorro2Board;

static BellatrixZorro2Board g_boards[BELLATRIX_MAX_ZORRO2_BOARDS];
static size_t g_board_count;
static int g_current_board;
static int g_initialized;

static uint8_t g_fast_ram_enabled;
static uint32_t g_fast_ram_size = 0x00800000u;
static uint8_t g_fast_ram_config[BELLATRIX_ZORRO2_CONFIG_BYTES];

static uint8_t g_cd_board_enabled;
static uint8_t g_cd_board_config[BELLATRIX_ZORRO2_CONFIG_BYTES];
static BellatrixZorro2BoardDesc g_cd_board_desc;

static void zorro2_emit_word_config(uint8_t *out, const uint16_t *words)
{
    size_t i;

    for (i = 0; i < BELLATRIX_ZORRO2_CONFIG_WORDS; ++i) {
        out[i * 2] = (uint8_t)(words[i] >> 8);
        out[i * 2 + 1] = (uint8_t)(words[i] & 0xFFu);
    }
}

static void zorro2_build_fast_ram_config(void)
{
    static const uint16_t words[BELLATRIX_ZORRO2_CONFIG_WORDS] = {
        0xe000u, 0x0000u,
        (uint16_t)(~(AUTOCONFIG_PRODUCT_ID << 8) & 0xf000u),
        (uint16_t)(~(AUTOCONFIG_PRODUCT_ID << 12) & 0xf000u),
        (uint16_t)(~0x8fffu), (uint16_t)(~0x0fffu),
        (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
        (uint16_t)(~AUTOCONFIG_MANUFACTURER_ID & 0xf000u),
        (uint16_t)(~(AUTOCONFIG_MANUFACTURER_ID << 4) & 0xf000u),
        (uint16_t)(~(AUTOCONFIG_MANUFACTURER_ID << 8) & 0xf000u),
        (uint16_t)(~(AUTOCONFIG_MANUFACTURER_ID << 12) & 0xf000u),
        (uint16_t)(~(AUTOCONFIG_RAM_SERIAL >> 16) & 0xf000u),
        (uint16_t)(~(AUTOCONFIG_RAM_SERIAL >> 12) & 0xf000u),
        (uint16_t)(~(AUTOCONFIG_RAM_SERIAL >> 8) & 0xf000u),
        (uint16_t)(~(AUTOCONFIG_RAM_SERIAL >> 4) & 0xf000u),
        (uint16_t)(~AUTOCONFIG_RAM_SERIAL & 0xf000u),
        (uint16_t)(~((uint16_t)AUTOCONFIG_RAM_SERIAL << 4) & 0xf000u),
        (uint16_t)(~((uint16_t)AUTOCONFIG_RAM_SERIAL << 8) & 0xf000u),
        (uint16_t)(~((uint16_t)AUTOCONFIG_RAM_SERIAL << 12) & 0xf000u),
        (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
        (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
        (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
        (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
        (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
        (uint16_t)(~0x0fffu), (uint16_t)(~0x0fffu),
    };

    zorro2_emit_word_config(g_fast_ram_config, words);
}

static void zorro2_build_cd_board_config(void)
{
    static const uint16_t words[BELLATRIX_ZORRO2_CONFIG_WORDS] = {
        (uint16_t)((0xD1u << 8) & 0xF000u),
        (uint16_t)((0xD1u << 12) & 0xF000u),
        (uint16_t)(~(0x11u << 8) & 0xF000u),
        (uint16_t)(~(0x11u << 12) & 0xF000u),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x6D73u & 0xF000u),
        (uint16_t)(~(0x6D73u << 4) & 0xF000u),
        (uint16_t)(~(0x6D73u << 8) & 0xF000u),
        (uint16_t)(~(0x6D73u << 12) & 0xF000u),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~(0x10u << 8) & 0xF000u),
        (uint16_t)(~(0x10u << 12) & 0xF000u),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
        (uint16_t)(~0x0FFFu), (uint16_t)(~0x0FFFu),
    };

    zorro2_emit_word_config(g_cd_board_config, words);
}

static int zorro2_find_slot(const char *id)
{
    size_t i;

    if (!id) {
        return -1;
    }

    for (i = 0; i < g_board_count; ++i) {
        if (g_boards[i].desc.id && strcmp(g_boards[i].desc.id, id) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int zorro2_first_visible_board(void)
{
    size_t i;

    for (i = 0; i < g_board_count; ++i) {
        BellatrixZorro2Board *board = &g_boards[i];

        if (board->enabled && !board->configured && !board->shutup) {
            return (int)i;
        }
    }

    return -1;
}

static void zorro2_refresh_current_board(void)
{
    g_current_board = zorro2_first_visible_board();

    if (g_current_board < 0) {
        kprintf("[ZORRO2] no further boards\n");
    }
}

static int zorro2_register_or_update(const BellatrixZorro2BoardDesc *desc)
{
    BellatrixZorro2Board *board;
    int slot;

    if (!desc || !desc->id || !desc->config_data || desc->config_size == 0) {
        return -1;
    }

    slot = zorro2_find_slot(desc->id);
    if (slot >= 0) {
        board = &g_boards[slot];
        board->desc = *desc;
        board->enabled = 1;
        board->configured = 0;
        board->shutup = 0;
        board->assigned_base = 0;
        if (g_initialized) {
            zorro2_refresh_current_board();
        }
        return 0;
    }

    if (g_board_count >= BELLATRIX_MAX_ZORRO2_BOARDS) {
        return -2;
    }

    board = &g_boards[g_board_count++];
    memset(board, 0, sizeof(*board));
    board->desc = *desc;
    board->enabled = 1;

    if (g_initialized && g_current_board < 0) {
        g_current_board = (int)(g_board_count - 1);
    }

    return 0;
}

static BellatrixZorro2Board *zorro2_current_board(void)
{
    if (g_current_board < 0 || (size_t)g_current_board >= g_board_count) {
        return NULL;
    }

    return &g_boards[g_current_board];
}

static BellatrixZorro2Board *zorro2_find_board_for_addr(uint32_t addr)
{
    size_t i;

    for (i = 0; i < g_board_count; ++i) {
        BellatrixZorro2Board *board = &g_boards[i];
        uint32_t base;
        uint32_t size;

        if (!board->enabled || !board->configured) {
            continue;
        }

        base = board->assigned_base;
        size = board->desc.window_size;
        if (!base || !size) {
            continue;
        }

        if (addr >= base && addr < base + size) {
            return board;
        }
    }

    return NULL;
}

static uint32_t zorro2_offset(uint32_t addr)
{
    return addr & AUTOCONFIG_Z2_MASK;
}

void bellatrix_zorro2_init(BellatrixMemory *memory)
{
    size_t i;

    (void)memory;

    for (i = 0; i < g_board_count; ++i) {
        g_boards[i].configured = 0;
        g_boards[i].shutup = 0;
        g_boards[i].assigned_base = 0;
    }

    g_initialized = 1;
    zorro2_refresh_current_board();

    if (!g_fast_ram_enabled && !g_cd_board_enabled && g_board_count == 0) {
        kprintf("[ZORRO2] disabled; config window=%08x returns empty\n",
                (unsigned)AUTOCONFIG_Z2_BASE);
    }
}

void bellatrix_zorro2_reset(BellatrixMemory *memory)
{
    size_t i;

    bellatrix_zorro2_init(memory);

    for (i = 0; i < g_board_count; ++i) {
        BellatrixZorro2Board *board = &g_boards[i];

        if (board->enabled && board->desc.ops && board->desc.ops->reset) {
            board->desc.ops->reset(board->desc.userdata);
        }
    }
}

int bellatrix_zorro2_register_board(const BellatrixZorro2BoardDesc *desc)
{
    return zorro2_register_or_update(desc);
}

int bellatrix_zorro2_unregister_board(const char *id)
{
    size_t i;
    int slot;

    slot = zorro2_find_slot(id);
    if (slot < 0) {
        return -1;
    }

    if (g_boards[slot].desc.ops && g_boards[slot].desc.ops->destroy) {
        g_boards[slot].desc.ops->destroy(g_boards[slot].desc.userdata);
    }

    for (i = (size_t)slot + 1; i < g_board_count; ++i) {
        g_boards[i - 1] = g_boards[i];
    }

    if (g_board_count > 0) {
        memset(&g_boards[g_board_count - 1], 0, sizeof(g_boards[0]));
        --g_board_count;
    }

    if (g_initialized) {
        zorro2_refresh_current_board();
    }

    return 0;
}

void bellatrix_zorro2_enable_fast_ram(uint32_t size_bytes)
{
    BellatrixZorro2BoardDesc desc;

    zorro2_build_fast_ram_config();

    g_fast_ram_enabled = 1;
    g_fast_ram_size = size_bytes ? size_bytes : 0x00800000u;

    memset(&desc, 0, sizeof(desc));
    desc.id = "legacy-z2-fast-ram";
    desc.config_data = g_fast_ram_config;
    desc.config_size = sizeof(g_fast_ram_config);
    desc.window_size = g_fast_ram_size;

    zorro2_register_or_update(&desc);
}

void bellatrix_zorro2_enable_cd_board(const uint8_t *rom, size_t rom_size)
{
    zorro2_build_cd_board_config();

    g_cd_board_enabled = 1;
    memset(&g_cd_board_desc, 0, sizeof(g_cd_board_desc));
    g_cd_board_desc.id = "legacy-cd-autoboot";
    g_cd_board_desc.config_data = g_cd_board_config;
    g_cd_board_desc.config_size = sizeof(g_cd_board_config);
    g_cd_board_desc.window_size = BELLATRIX_ZORRO2_CD_BOARD_WINDOW;
    g_cd_board_desc.rom = rom;
    g_cd_board_desc.rom_size = rom_size;

    zorro2_register_or_update(&g_cd_board_desc);

    kprintf("[ZORRO2] enabled CD board (DiagArea ROM %zu B)\n", rom_size);
}

int bellatrix_zorro2_config_window_contains(uint32_t addr)
{
    return addr >= BELLATRIX_Z2_CONFIG_BASE && addr <= BELLATRIX_Z2_CONFIG_END;
}

int bellatrix_zorro2_board_window_contains(uint32_t addr)
{
    return zorro2_find_board_for_addr(addr) != NULL;
}

uint8_t bellatrix_zorro2_config_read8(BellatrixMemory *memory, uint32_t addr)
{
    BellatrixZorro2Board *board;
    uint32_t off;

    (void)memory;

    board = zorro2_current_board();
    if (!board) {
        return 0xFFu;
    }

    off = zorro2_offset(addr);
    if (off >= board->desc.config_size) {
        return 0xFFu;
    }

    return board->desc.config_data[off];
}

uint16_t bellatrix_zorro2_config_read16(BellatrixMemory *memory, uint32_t addr)
{
    uint16_t hi = bellatrix_zorro2_config_read8(memory, addr);
    uint16_t lo = bellatrix_zorro2_config_read8(memory, addr + 1);

    return (uint16_t)((hi << 8) | lo);
}

uint32_t bellatrix_zorro2_config_read32(BellatrixMemory *memory, uint32_t addr)
{
    uint32_t b0 = bellatrix_zorro2_config_read8(memory, addr);
    uint32_t b1 = bellatrix_zorro2_config_read8(memory, addr + 1);
    uint32_t b2 = bellatrix_zorro2_config_read8(memory, addr + 2);
    uint32_t b3 = bellatrix_zorro2_config_read8(memory, addr + 3);

    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

void bellatrix_zorro2_config_write8(BellatrixMemory *memory, uint32_t addr, uint8_t value)
{
    BellatrixZorro2Board *board;
    uint32_t off;

    (void)memory;

    board = zorro2_current_board();
    if (!board) {
        return;
    }

    off = zorro2_offset(addr);

    switch (off) {
    case AUTOCONFIG_REG_ASSIGN_HI:
        board->assigned_base = ((uint32_t)value << 16);
        board->configured = 1;
        kprintf("[ZORRO2] assigned board %s base=%08x size=%08x\n",
                board->desc.id ? board->desc.id : "<unnamed>",
                (unsigned)board->assigned_base,
                (unsigned)board->desc.window_size);
        zorro2_refresh_current_board();
        break;

    case AUTOCONFIG_REG_SHUTUP:
        board->shutup = 1;
        kprintf("[ZORRO2] shutup board %s\n",
                board->desc.id ? board->desc.id : "<unnamed>");
        zorro2_refresh_current_board();
        break;

    case (AUTOCONFIG_REG_ASSIGN_HI + 2u):
    case (AUTOCONFIG_REG_SHUTUP + 2u):
        break;

    default:
        break;
    }
}

void bellatrix_zorro2_config_write16(BellatrixMemory *memory, uint32_t addr, uint16_t value)
{
    bellatrix_zorro2_config_write8(memory, addr, (uint8_t)(value >> 8));
    bellatrix_zorro2_config_write8(memory, addr + 1, (uint8_t)(value & 0xFFu));
}

void bellatrix_zorro2_config_write32(BellatrixMemory *memory, uint32_t addr, uint32_t value)
{
    bellatrix_zorro2_config_write8(memory, addr, (uint8_t)(value >> 24));
    bellatrix_zorro2_config_write8(memory, addr + 1, (uint8_t)(value >> 16));
    bellatrix_zorro2_config_write8(memory, addr + 2, (uint8_t)(value >> 8));
    bellatrix_zorro2_config_write8(memory, addr + 3, (uint8_t)(value & 0xFFu));
}

uint8_t bellatrix_zorro2_board_read8(uint32_t addr)
{
    BellatrixZorro2Board *board;
    uint32_t off;

    board = zorro2_find_board_for_addr(addr);
    if (!board) {
        return 0xFFu;
    }

    off = addr - board->assigned_base;

    if (board->desc.rom && off < board->desc.rom_size) {
        return board->desc.rom[off];
    }

    if (board->desc.ops && board->desc.ops->read8) {
        return board->desc.ops->read8(board->desc.userdata, off);
    }

    return 0xFFu;
}

uint16_t bellatrix_zorro2_board_read16(uint32_t addr)
{
    return (uint16_t)(((uint16_t)bellatrix_zorro2_board_read8(addr) << 8) |
                      bellatrix_zorro2_board_read8(addr + 1));
}

uint32_t bellatrix_zorro2_board_read32(uint32_t addr)
{
    return ((uint32_t)bellatrix_zorro2_board_read8(addr) << 24) |
           ((uint32_t)bellatrix_zorro2_board_read8(addr + 1) << 16) |
           ((uint32_t)bellatrix_zorro2_board_read8(addr + 2) << 8) |
            (uint32_t)bellatrix_zorro2_board_read8(addr + 3);
}

int bellatrix_zorro2_board_configured(const char *id)
{
    int slot = zorro2_find_slot(id);

    if (slot < 0) {
        return 0;
    }

    return g_boards[slot].configured;
}

uint32_t bellatrix_zorro2_board_base(const char *id)
{
    int slot = zorro2_find_slot(id);

    if (slot < 0) {
        return 0;
    }

    return g_boards[slot].assigned_base;
}
