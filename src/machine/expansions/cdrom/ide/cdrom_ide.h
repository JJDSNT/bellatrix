#ifndef BELLATRIX_EXPANSIONS_CDROM_IDE_H
#define BELLATRIX_EXPANSIONS_CDROM_IDE_H

#include <stdint.h>

typedef struct BellatrixCdromIdeState {
    uint8_t regs[256];
} BellatrixCdromIdeState;


void bellatrix_cdrom_ide_init(BellatrixCdromIdeState *ide);
void bellatrix_cdrom_ide_reset(BellatrixCdromIdeState *ide);
uint8_t bellatrix_cdrom_ide_read8(BellatrixCdromIdeState *ide, uint32_t offset);
void bellatrix_cdrom_ide_write8(
    BellatrixCdromIdeState *ide,
    uint32_t offset,
    uint8_t value
);

#endif
