#ifndef BELLATRIX_EXPANSIONS_CDROM_STATE_H
#define BELLATRIX_EXPANSIONS_CDROM_STATE_H

#include "machine/bus/zorro2/zorro2_bus.h"
#include "machine/expansions/cdrom/ide/cdrom_ide.h"
#include "machine/expansions/cdrom/media/cdrom_atapi.h"
#include "machine/expansions/cdrom/media/cdrom_device.h"
#include "machine/expansions/cdrom/media/cdrom_iso_backend.h"

#include <stddef.h>
#include <stdint.h>

typedef struct BellatrixCdromPluginState {
    char plugin_root[512];
    char resident_rom_path[768];

    uint8_t *resident_rom;
    size_t resident_rom_size;
    uint8_t config_data[BELLATRIX_ZORRO2_CONFIG_BYTES];

    BellatrixCdromIdeState ide;
    BellatrixCdromDevice device;
    BellatrixCdromIsoBackend iso_backend;
    BellatrixCdromMediaOps media_ops;
    BellatrixAtapiCdrom atapi;
    BellatrixZorro2BoardDesc board_desc;
} BellatrixCdromPluginState;

#endif
