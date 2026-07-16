#ifndef BELLATRIX_BUS_ZORRO3_H
#define BELLATRIX_BUS_ZORRO3_H

#include <stddef.h>
#include <stdint.h>

#include "machine/autoconfig/autoconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Zorro 3 boards share the $E80000 Autoconfig window with Z2. Following the
 * Emu68 vectors contract, a write at offset 0x44 supplies board_base[31:16]
 * and completes assignment. Do not impose an OS-specific allocation window:
 * the board/backend decides whether it can map the guest-assigned base.
 */

typedef struct BellatrixZorro3BoardOps {
    int     (*map)(void *userdata, uint32_t base, uint32_t size);
    void    (*unmap)(void *userdata, uint32_t base, uint32_t size);
    void    (*reset)(void *userdata);
    void    (*destroy)(void *userdata);
    uint8_t (*read8)(void *userdata, uint32_t offset);
    void    (*write8)(void *userdata, uint32_t offset, uint8_t value);
} BellatrixZorro3BoardOps;

typedef struct BellatrixZorro3BoardDesc {
    const char *id;
    const uint8_t *config_data;  /* AUTOCONFIG_DATA_SIZE bytes, nibble-encoded */
    size_t config_size;
    uint32_t window_size;
    void *userdata;
    const BellatrixZorro3BoardOps *ops;
} BellatrixZorro3BoardDesc;

void bellatrix_zorro3_init(void);
void bellatrix_zorro3_reset(void);

int  bellatrix_zorro3_register_board(const BellatrixZorro3BoardDesc *desc);
int  bellatrix_zorro3_unregister_board(const char *id);

int bellatrix_zorro3_has_pending_board(void);

uint8_t  bellatrix_zorro3_config_read8 (uint32_t addr);
uint16_t bellatrix_zorro3_config_read16(uint32_t addr);
uint32_t bellatrix_zorro3_config_read32(uint32_t addr);

void bellatrix_zorro3_config_write8 (uint32_t addr, uint8_t value);
void bellatrix_zorro3_config_write16(uint32_t addr, uint16_t value);
void bellatrix_zorro3_config_write32(uint32_t addr, uint32_t value);

int bellatrix_zorro3_in_board_window(uint32_t addr);

uint8_t  bellatrix_zorro3_board_read8 (uint32_t addr);
uint16_t bellatrix_zorro3_board_read16(uint32_t addr);
uint32_t bellatrix_zorro3_board_read32(uint32_t addr);

void bellatrix_zorro3_board_write8 (uint32_t addr, uint8_t  value);
void bellatrix_zorro3_board_write16(uint32_t addr, uint16_t value);
void bellatrix_zorro3_board_write32(uint32_t addr, uint32_t value);

int      bellatrix_zorro3_board_configured(const char *id);
uint32_t bellatrix_zorro3_board_base(const char *id);

#ifdef __cplusplus
}
#endif

#endif
