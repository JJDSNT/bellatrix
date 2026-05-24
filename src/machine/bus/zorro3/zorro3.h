#ifndef BELLATRIX_BUS_ZORRO3_H
#define BELLATRIX_BUS_ZORRO3_H

#include <stddef.h>
#include <stdint.h>

#include "machine/autoconfig/autoconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Zorro 3 boards use the same $E80000 config window as Z2 but with
 * er_Type bits 7-6 = 10. The base address is assigned via two writes:
 *   offset 0x44 (AC_OFF_Z3_HI): board_base[31:24]
 *   offset 0x48 (AC_OFF_BASE_HI): board_base[23:16]  (also triggers assignment)
 * Board windows live at $40000000–$7FFFFFFF.
 */

#define Z3_WINDOW_BASE  0x40000000u
#define Z3_WINDOW_END   0x7FFFFFFFu

typedef struct BellatrixZorro3BoardOps {
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
