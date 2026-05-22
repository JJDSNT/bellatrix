#ifndef BELLATRIX_PLUGIN_REGISTRY_H
#define BELLATRIX_PLUGIN_REGISTRY_H

#include "plugin/plugin_types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BELLATRIX_MAX_PLUGINS
#define BELLATRIX_MAX_PLUGINS 64
#endif

typedef struct BellatrixPluginRegistry {
    BellatrixPlugin *plugins[BELLATRIX_MAX_PLUGINS];
    size_t count;
} BellatrixPluginRegistry;

/*
 * Global plugin registry access.
 */

BellatrixPluginRegistry *bellatrix_plugin_registry(void);

/*
 * Register loaded plugin.
 */

int bellatrix_plugin_registry_add(
    BellatrixPlugin *plugin
);

/*
 * Find plugin by id.
 */

BellatrixPlugin *bellatrix_plugin_registry_find(
    const char *id
);

/*
 * Remove all plugins.
 */

void bellatrix_plugin_registry_clear(void);

#ifdef __cplusplus
}
#endif

#endif