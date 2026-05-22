#include "machine/expansions/cdrom/ide/cdrom_ide.h"

#include <string.h>

void bellatrix_cdrom_ide_init(BellatrixCdromIdeState *ide)
{
    if (!ide) {
        return;
    }

    memset(ide, 0, sizeof(*ide));

    ide->regs[0x00] = 0xCDu;
    ide->regs[0x01] = 0x42u;
    ide->regs[0x07] = 0x40u;
}

void bellatrix_cdrom_ide_reset(BellatrixCdromIdeState *ide)
{
    bellatrix_cdrom_ide_init(ide);
}

uint8_t bellatrix_cdrom_ide_read8(BellatrixCdromIdeState *ide, uint32_t offset)
{
    if (!ide) {
        return 0xFFu;
    }

    return ide->regs[offset & 0xFFu];
}

void bellatrix_cdrom_ide_write8(
    BellatrixCdromIdeState *ide,
    uint32_t offset,
    uint8_t value
)
{
    if (!ide) {
        return;
    }

    ide->regs[offset & 0xFFu] = value;
}
