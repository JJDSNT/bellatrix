#include "plugin/plugin_registry.h"

#include <stdlib.h>
#include <string.h>

static BellatrixPluginRegistry g_plugin_registry;

BellatrixPluginRegistry *bellatrix_plugin_registry(void)
{
    return &g_plugin_registry;
}

int bellatrix_plugin_registry_add(
    BellatrixPlugin *plugin
)
{
    BellatrixPluginRegistry *registry;
    size_t i;

    if (!plugin || !plugin->id) {
        return -1;
    }

    registry = bellatrix_plugin_registry();

    if (registry->count >= BELLATRIX_MAX_PLUGINS) {
        return -2;
    }

    for (i = 0; i < registry->count; ++i) {
        BellatrixPlugin *existing = registry->plugins[i];

        if (!existing || !existing->id) {
            continue;
        }

        if (strcmp(existing->id, plugin->id) == 0) {
            return -3;
        }
    }

    registry->plugins[registry->count++] = plugin;

    return 0;
}

BellatrixPlugin *bellatrix_plugin_registry_find(
    const char *id
)
{
    BellatrixPluginRegistry *registry;
    size_t i;

    if (!id) {
        return NULL;
    }

    registry = bellatrix_plugin_registry();

    for (i = 0; i < registry->count; ++i) {
        BellatrixPlugin *plugin = registry->plugins[i];

        if (!plugin || !plugin->id) {
            continue;
        }

        if (strcmp(plugin->id, id) == 0) {
            return plugin;
        }
    }

    return NULL;
}

void bellatrix_plugin_registry_clear(void)
{
    BellatrixPluginRegistry *registry;
    size_t i;

    registry = bellatrix_plugin_registry();

    for (i = 0; i < registry->count; ++i) {

        BellatrixPlugin *plugin = registry->plugins[i];

        if (!plugin) {
            continue;
        }

#if !defined(_WIN32)
        /*
         * Future:
         * dlclose(plugin->dl_handle);
         */
#endif

        free(plugin);

        registry->plugins[i] = NULL;
    }

    registry->count = 0;
}