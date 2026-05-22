#ifndef BELLATRIX_PLUGIN_LOADER_H
#define BELLATRIX_PLUGIN_LOADER_H

#include "plugin/plugin_api.h"
#include "plugin/plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BellatrixPluginLoader {
    const char *search_path;
    BellatrixPluginApi api;
} BellatrixPluginLoader;

/*
 * Initialize plugin loader.
 *
 * Example:
 *
 *     bellatrix_plugin_loader_init(
 *         &loader,
 *         "expansions"
 *     );
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
 * Load every plugin found in:
 *
 *     expansions/*
 */

int bellatrix_plugin_load_all(
    BellatrixPluginLoader *loader,
    BellatrixMachine *machine
);

/*
 * Load a single expansion directory.
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