#ifndef BELLATRIX_BUS_ZORRO2_BUS_H
#define BELLATRIX_BUS_ZORRO2_BUS_H

#include <stddef.h>
#include <stdint.h>

#include "machine/autoconfig/autoconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board window size constants */
#define BELLATRIX_ZORRO2_WIN_64KB    0x00010000u
#define BELLATRIX_ZORRO2_WIN_128KB   0x00020000u
#define BELLATRIX_ZORRO2_WIN_256KB   0x00040000u
#define BELLATRIX_ZORRO2_WIN_512KB   0x00080000u
#define BELLATRIX_ZORRO2_WIN_1MB     0x00100000u
#define BELLATRIX_ZORRO2_WIN_2MB     0x00200000u
#define BELLATRIX_ZORRO2_WIN_8MB     0x00800000u

/* Backward-compat alias used by existing expansions */
#define BELLATRIX_ZORRO2_CONFIG_BYTES     AUTOCONFIG_DATA_SIZE
#define BELLATRIX_ZORRO2_CONFIG_WORDS    (AUTOCONFIG_DATA_SIZE / 2u)
#define BELLATRIX_ZORRO2_CD_BOARD_WINDOW  BELLATRIX_ZORRO2_WIN_64KB

typedef struct BellatrixZorro2BoardOps {
    void    (*reset)(void *userdata);
    void    (*destroy)(void *userdata);
    uint8_t (*read8)(void *userdata, uint32_t offset);
    void    (*write8)(void *userdata, uint32_t offset, uint8_t value);
} BellatrixZorro2BoardOps;

typedef struct BellatrixZorro2BoardDesc {
    const char *id;
    const uint8_t *config_data;   /* AUTOCONFIG_DATA_SIZE bytes, nibble-encoded */
    size_t config_size;
    uint32_t window_size;
    const uint8_t *rom;           /* optional board ROM (served at board base)  */
    size_t rom_size;
    void *userdata;
    const BellatrixZorro2BoardOps *ops;
} BellatrixZorro2BoardDesc;

void bellatrix_zorro2_init(void);
void bellatrix_zorro2_reset(void);

int  bellatrix_zorro2_register_board(const BellatrixZorro2BoardDesc *desc);
int  bellatrix_zorro2_unregister_board(const char *id);

/* Returns 1 if address falls in the autoconfig config window */
int bellatrix_zorro2_in_config_window(uint32_t addr);

/* Returns 1 if address falls inside a configured board window */
int bellatrix_zorro2_in_board_window(uint32_t addr);

/* Config window access (called from machine.c for $E80000-$E8FFFF) */
uint8_t  bellatrix_zorro2_config_read8 (uint32_t addr);
uint16_t bellatrix_zorro2_config_read16(uint32_t addr);
uint32_t bellatrix_zorro2_config_read32(uint32_t addr);

void bellatrix_zorro2_config_write8 (uint32_t addr, uint8_t  value);
void bellatrix_zorro2_config_write16(uint32_t addr, uint16_t value);
void bellatrix_zorro2_config_write32(uint32_t addr, uint32_t value);

/* Board window access (called from machine.c for assigned board ranges) */
uint8_t  bellatrix_zorro2_board_read8 (uint32_t addr);
uint16_t bellatrix_zorro2_board_read16(uint32_t addr);
uint32_t bellatrix_zorro2_board_read32(uint32_t addr);

void bellatrix_zorro2_board_write8 (uint32_t addr, uint8_t  value);
void bellatrix_zorro2_board_write16(uint32_t addr, uint16_t value);
void bellatrix_zorro2_board_write32(uint32_t addr, uint32_t value);

int      bellatrix_zorro2_board_configured(const char *id);
uint32_t bellatrix_zorro2_board_base(const char *id);

/* Register a Zorro2 Fast RAM board for the legacy (non-boards) path.
 * size_bytes is clamped to the nearest supported AC size (max 8 MB). */
void bellatrix_zorro2_enable_fast_ram(uint32_t size_bytes);

/* Fast RAM board state: registered = enable_fast_ram() was called;
 * configured = autoconfig assigned it a base (RAM may respond). */
int bellatrix_zorro2_fast_ram_registered(void);
int bellatrix_zorro2_fast_ram_configured(void);

#ifdef __cplusplus
}
#endif

#endif
