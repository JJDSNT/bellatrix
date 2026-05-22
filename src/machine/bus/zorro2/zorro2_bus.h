#ifndef BELLATRIX_BUS_ZORRO2_BUS_H
#define BELLATRIX_BUS_ZORRO2_BUS_H

#include <stddef.h>
#include <stdint.h>

#include "machine/memory/memory.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BELLATRIX_ZORRO2_CONFIG_SIZE     0x00010000u
#define BELLATRIX_ZORRO2_CONFIG_WORDS    32u
#define BELLATRIX_ZORRO2_CONFIG_BYTES    (BELLATRIX_ZORRO2_CONFIG_WORDS * 2u)
#define BELLATRIX_ZORRO2_CD_BOARD_WINDOW 0x00010000u
#define AUTOCONFIG_CD_BOARD_WINDOW       BELLATRIX_ZORRO2_CD_BOARD_WINDOW

typedef struct BellatrixZorro2BoardOps {
    void (*reset)(void *userdata);
    void (*destroy)(void *userdata);
    uint8_t (*read8)(void *userdata, uint32_t offset);
    void (*write8)(void *userdata, uint32_t offset, uint8_t value);
} BellatrixZorro2BoardOps;

typedef struct BellatrixZorro2BoardDesc {
    const char *id;
    const uint8_t *config_data;
    size_t config_size;
    uint32_t window_size;
    const uint8_t *rom;
    size_t rom_size;
    void *userdata;
    const BellatrixZorro2BoardOps *ops;
} BellatrixZorro2BoardDesc;

void bellatrix_zorro2_init(BellatrixMemory *memory);
void bellatrix_zorro2_reset(BellatrixMemory *memory);

int bellatrix_zorro2_register_board(const BellatrixZorro2BoardDesc *desc);
int bellatrix_zorro2_unregister_board(const char *id);

void bellatrix_zorro2_enable_fast_ram(uint32_t size_bytes);
void bellatrix_zorro2_enable_cd_board(const uint8_t *rom, size_t rom_size);

int bellatrix_zorro2_config_window_contains(uint32_t addr);
int bellatrix_zorro2_board_window_contains(uint32_t addr);

uint8_t bellatrix_zorro2_config_read8(BellatrixMemory *memory, uint32_t addr);
uint16_t bellatrix_zorro2_config_read16(BellatrixMemory *memory, uint32_t addr);
uint32_t bellatrix_zorro2_config_read32(BellatrixMemory *memory, uint32_t addr);

void bellatrix_zorro2_config_write8(BellatrixMemory *memory, uint32_t addr, uint8_t value);
void bellatrix_zorro2_config_write16(BellatrixMemory *memory, uint32_t addr, uint16_t value);
void bellatrix_zorro2_config_write32(BellatrixMemory *memory, uint32_t addr, uint32_t value);

uint8_t bellatrix_zorro2_board_read8(uint32_t addr);
uint16_t bellatrix_zorro2_board_read16(uint32_t addr);
uint32_t bellatrix_zorro2_board_read32(uint32_t addr);

int bellatrix_zorro2_board_configured(const char *id);
uint32_t bellatrix_zorro2_board_base(const char *id);

#ifdef __cplusplus
}
#endif

#endif
