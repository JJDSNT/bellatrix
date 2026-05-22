#ifndef BELLATRIX_EXPANSIONS_CDROM_PLUGIN_H
#define BELLATRIX_EXPANSIONS_CDROM_PLUGIN_H

#include "core/expansion.h"
#include "plugin/plugin_api.h"

int bellatrix_cdrom_plugin_register(
    BellatrixMachine *machine,
    const BellatrixPluginApi *api
);

#endif
