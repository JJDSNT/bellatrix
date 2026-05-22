#ifndef BELLATRIX_EXPANSIONS_CDROM_BOARD_H
#define BELLATRIX_EXPANSIONS_CDROM_BOARD_H

#include "expansions/cdrom/cdrom_state.h"

int bellatrix_cdrom_board_init(
    BellatrixCdromPluginState *state,
    const char *plugin_root
);

void bellatrix_cdrom_board_destroy(
    BellatrixCdromPluginState *state
);

#endif
