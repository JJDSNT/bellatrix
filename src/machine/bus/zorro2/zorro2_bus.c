#include "machine/bus/zorro2/zorro2_bus.h"

#include "support.h"

#include <string.h>

#ifndef BELLATRIX_MAX_ZORRO2_BOARDS
#define BELLATRIX_MAX_ZORRO2_BOARDS 16
#endif

/* $E80000–$E8FFFF: autoconfig config window */
#define Z2_CONFIG_BASE  0x00E80000u
#define Z2_CONFIG_END   0x00E8FFFFu
#define Z2_CONFIG_MASK  0x0000FFFFu   /* offset within config window */

typedef struct Zorro2Board {
    BellatrixZorro2BoardDesc desc;
    uint8_t  enabled;
    uint8_t  configured;
    uint8_t  shutup;
    uint32_t base;
} Zorro2Board;

static Zorro2Board  s_boards[BELLATRIX_MAX_ZORRO2_BOARDS];
static size_t       s_count;
static int          s_current;   /* index of board being configured, or -1 */

/* ----------------------------------------------------------------------- */

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
    if (s_current < 0)
        kprintf("[Z2] all boards configured\n");
}

static Zorro2Board *current_board(void)
{
    if (s_current < 0 || (size_t)s_current >= s_count)
        return NULL;
    return &s_boards[s_current];
}

static Zorro2Board *board_for_addr(uint32_t addr)
{
    size_t i;
    for (i = 0; i < s_count; ++i) {
        Zorro2Board *b = &s_boards[i];
        if (!b->enabled || !b->configured || !b->base || !b->desc.window_size)
            continue;
        if (addr >= b->base && addr < b->base + b->desc.window_size)
            return b;
    }
    return NULL;
}

/* ----------------------------------------------------------------------- */

void bellatrix_zorro2_init(void)
{
    size_t i;
    for (i = 0; i < s_count; ++i) {
        s_boards[i].configured = 0;
        s_boards[i].shutup     = 0;
        s_boards[i].base       = 0;
    }
    refresh_current();
}

void bellatrix_zorro2_reset(void)
{
    size_t i;
    bellatrix_zorro2_init();
    for (i = 0; i < s_count; ++i) {
        Zorro2Board *b = &s_boards[i];
        if (b->enabled && b->desc.ops && b->desc.ops->reset)
            b->desc.ops->reset(b->desc.userdata);
    }
}

int bellatrix_zorro2_register_board(const BellatrixZorro2BoardDesc *desc)
{
    Zorro2Board *b;
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
        refresh_current();
        return 0;
    }

    if (s_count >= BELLATRIX_MAX_ZORRO2_BOARDS)
        return -2;

    b = &s_boards[s_count++];
    memset(b, 0, sizeof(*b));
    b->desc    = *desc;
    b->enabled = 1;

    if (s_current < 0)
        s_current = (int)(s_count - 1);

    kprintf("[Z2] board registered: %s  window=%08x\n",
            desc->id, (unsigned)desc->window_size);
    return 0;
}

int bellatrix_zorro2_unregister_board(const char *id)
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

/* ----------------------------------------------------------------------- */

int bellatrix_zorro2_in_config_window(uint32_t addr)
{
    return (addr >= Z2_CONFIG_BASE && addr <= Z2_CONFIG_END);
}

int bellatrix_zorro2_in_board_window(uint32_t addr)
{
    return board_for_addr(addr) != NULL;
}

/* ----------------------------------------------------------------------- */

uint8_t bellatrix_zorro2_config_read8(uint32_t addr)
{
    Zorro2Board *b;
    uint32_t off;

    b = current_board();
    if (!b)
        return 0xFFu;

    off = addr & Z2_CONFIG_MASK;
    if (off >= b->desc.config_size)
        return 0xFFu;

    return b->desc.config_data[off];
}

uint16_t bellatrix_zorro2_config_read16(uint32_t addr)
{
    return (uint16_t)(((uint16_t)bellatrix_zorro2_config_read8(addr) << 8) |
                       bellatrix_zorro2_config_read8(addr + 1u));
}

uint32_t bellatrix_zorro2_config_read32(uint32_t addr)
{
    return ((uint32_t)bellatrix_zorro2_config_read8(addr)     << 24) |
           ((uint32_t)bellatrix_zorro2_config_read8(addr + 1) << 16) |
           ((uint32_t)bellatrix_zorro2_config_read8(addr + 2) <<  8) |
            (uint32_t)bellatrix_zorro2_config_read8(addr + 3);
}

void bellatrix_zorro2_config_write8(uint32_t addr, uint8_t value)
{
    Zorro2Board *b;
    uint32_t off;

    b = current_board();
    if (!b)
        return;

    off = addr & Z2_CONFIG_MASK;

    if (off == AC_OFF_BASE_HI) {
        b->base       = (uint32_t)value << 16;
        b->configured = 1;
        kprintf("[Z2] board '%s' assigned base=%08x size=%08x\n",
                b->desc.id ? b->desc.id : "?",
                (unsigned)b->base,
                (unsigned)b->desc.window_size);
        refresh_current();
    } else if (off == AC_OFF_SHUTUP) {
        b->shutup = 1;
        kprintf("[Z2] board '%s' shutup\n",
                b->desc.id ? b->desc.id : "?");
        refresh_current();
    }
}

void bellatrix_zorro2_config_write16(uint32_t addr, uint16_t value)
{
    bellatrix_zorro2_config_write8(addr,      (uint8_t)(value >> 8));
    bellatrix_zorro2_config_write8(addr + 1u, (uint8_t)(value & 0xFFu));
}

void bellatrix_zorro2_config_write32(uint32_t addr, uint32_t value)
{
    bellatrix_zorro2_config_write8(addr,      (uint8_t)(value >> 24));
    bellatrix_zorro2_config_write8(addr + 1u, (uint8_t)(value >> 16));
    bellatrix_zorro2_config_write8(addr + 2u, (uint8_t)(value >>  8));
    bellatrix_zorro2_config_write8(addr + 3u, (uint8_t)(value & 0xFFu));
}

/* ----------------------------------------------------------------------- */

uint8_t bellatrix_zorro2_board_read8(uint32_t addr)
{
    Zorro2Board *b = board_for_addr(addr);
    uint32_t off;

    if (!b)
        return 0xFFu;

    off = addr - b->base;

    if (b->desc.rom && off < b->desc.rom_size)
        return b->desc.rom[off];

    if (b->desc.ops && b->desc.ops->read8)
        return b->desc.ops->read8(b->desc.userdata, off);

    return 0xFFu;
}

uint16_t bellatrix_zorro2_board_read16(uint32_t addr)
{
    return (uint16_t)(((uint16_t)bellatrix_zorro2_board_read8(addr) << 8) |
                       bellatrix_zorro2_board_read8(addr + 1u));
}

uint32_t bellatrix_zorro2_board_read32(uint32_t addr)
{
    return ((uint32_t)bellatrix_zorro2_board_read8(addr)     << 24) |
           ((uint32_t)bellatrix_zorro2_board_read8(addr + 1) << 16) |
           ((uint32_t)bellatrix_zorro2_board_read8(addr + 2) <<  8) |
            (uint32_t)bellatrix_zorro2_board_read8(addr + 3);
}

void bellatrix_zorro2_board_write8(uint32_t addr, uint8_t value)
{
    Zorro2Board *b = board_for_addr(addr);
    uint32_t off;

    if (!b) return;
    off = addr - b->base;

    if (b->desc.ops && b->desc.ops->write8)
        b->desc.ops->write8(b->desc.userdata, off, value);
}

void bellatrix_zorro2_board_write16(uint32_t addr, uint16_t value)
{
    bellatrix_zorro2_board_write8(addr,      (uint8_t)(value >> 8));
    bellatrix_zorro2_board_write8(addr + 1u, (uint8_t)(value & 0xFFu));
}

void bellatrix_zorro2_board_write32(uint32_t addr, uint32_t value)
{
    bellatrix_zorro2_board_write8(addr,      (uint8_t)(value >> 24));
    bellatrix_zorro2_board_write8(addr + 1u, (uint8_t)(value >> 16));
    bellatrix_zorro2_board_write8(addr + 2u, (uint8_t)(value >>  8));
    bellatrix_zorro2_board_write8(addr + 3u, (uint8_t)(value & 0xFFu));
}

int bellatrix_zorro2_board_configured(const char *id)
{
    int slot = find_slot(id);
    if (slot < 0) return 0;
    return s_boards[slot].configured;
}

uint32_t bellatrix_zorro2_board_base(const char *id)
{
    int slot = find_slot(id);
    if (slot < 0) return 0;
    return s_boards[slot].base;
}

/* ----------------------------------------------------------------------- */

static uint8_t s_fast_ram_config[AUTOCONFIG_DATA_SIZE];
static BellatrixZorro2BoardDesc s_fast_ram_board_desc;

static uint8_t bytes_to_ac_size(uint32_t size)
{
    if (size >= 8u * 1024u * 1024u) return AC_SIZE_8MB;
    if (size >= 4u * 1024u * 1024u) return AC_SIZE_4MB;
    if (size >= 2u * 1024u * 1024u) return AC_SIZE_2MB;
    if (size >= 1u * 1024u * 1024u) return AC_SIZE_1MB;
    if (size >= 512u * 1024u)       return AC_SIZE_512KB;
    if (size >= 256u * 1024u)       return AC_SIZE_256KB;
    if (size >= 128u * 1024u)       return AC_SIZE_128KB;
    return AC_SIZE_64KB;
}

static uint32_t ac_size_to_bytes(uint8_t code)
{
    switch (code) {
        case AC_SIZE_8MB:   return 8u * 1024u * 1024u;
        case AC_SIZE_4MB:   return 4u * 1024u * 1024u;
        case AC_SIZE_2MB:   return 2u * 1024u * 1024u;
        case AC_SIZE_1MB:   return 1u * 1024u * 1024u;
        case AC_SIZE_512KB: return 512u * 1024u;
        case AC_SIZE_256KB: return 256u * 1024u;
        case AC_SIZE_128KB: return 128u * 1024u;
        default:            return 64u * 1024u;
    }
}

static int s_fast_ram_registered = 0;

int bellatrix_zorro2_fast_ram_registered(void)
{
    return s_fast_ram_registered;
}

int bellatrix_zorro2_fast_ram_configured(void)
{
    return s_fast_ram_registered &&
           bellatrix_zorro2_board_configured("bellatrix.fastram");
}

int bellatrix_zorro2_fast_ram_window(uint32_t *base, uint32_t *size)
{
    if (!bellatrix_zorro2_fast_ram_configured())
        return 0;
    if (base) *base = bellatrix_zorro2_board_base("bellatrix.fastram");
    if (size) *size = s_fast_ram_board_desc.window_size;
    return 1;
}

void bellatrix_zorro2_enable_fast_ram(uint32_t size_bytes)
{
    uint8_t sz  = bytes_to_ac_size(size_bytes);
    uint8_t raw[AUTOCONFIG_ROM_BYTES];

    memset(raw, 0, sizeof(raw));
    raw[0]  = (uint8_t)(AC_TYPE_Z2 | AC_TYPE_MEMLIST | sz);
    raw[1]  = 0x01u;               /* product ID */
    raw[4]  = 0x07u;               /* manufacturer 0x07DB high */
    raw[5]  = 0xDBu;               /* manufacturer 0x07DB low  */
    raw[9]  = 0x01u;               /* serial 1 */

    autoconfig_build(s_fast_ram_config, raw);

    memset(&s_fast_ram_board_desc, 0, sizeof(s_fast_ram_board_desc));
    s_fast_ram_board_desc.id          = "bellatrix.fastram";
    s_fast_ram_board_desc.config_data = s_fast_ram_config;
    s_fast_ram_board_desc.config_size = sizeof(s_fast_ram_config);
    s_fast_ram_board_desc.window_size = ac_size_to_bytes(sz);

    bellatrix_zorro2_register_board(&s_fast_ram_board_desc);
    s_fast_ram_registered = 1;
}
