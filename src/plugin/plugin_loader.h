#ifndef BELLATRIX_PLUGIN_LOADER_H
#define BELLATRIX_PLUGIN_LOADER_H

#include "plugin/plugin_types.h"
#include "plugin/plugin_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Plugin loader state.
 */

typedef struct BellatrixPluginLoader {
    const char *search_path;
    BellatrixPluginApi api;
} BellatrixPluginLoader;

/*
 * Initialize plugin loader.
 */

int bellatrix_plugin_loader_init(
    BellatrixPluginLoader *loader,
    const char *search_path
);

/*
 * Shutdown plugin loader.
 */

void bellatrix_plugin_loader_shutdown(
    BellatrixPluginLoader *loader
);

/*
 * Load all plugins found in search_path.
 */

int bellatrix_plugin_load_all(
    BellatrixPluginLoader *loader,
    BellatrixMachine *machine
);

/*
 * Load a single plugin directory.
 *
 * Example:
 *
 *     expansions/cdrom
 */

int bellatrix_plugin_load_directory(
    BellatrixPluginLoader *loader,
    BellatrixMachine *machine,
    const char *path
);

#ifdef __cplusplus
}
#endif

#endif