#ifndef BELLATRIX_PLUGIN_MANIFEST_H
#define BELLATRIX_PLUGIN_MANIFEST_H

#include "plugin/plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load plugin manifest from plugin.json.
 */

int bellatrix_plugin_manifest_load(
    const char *path,
    BellatrixPluginManifest *manifest
);

/*
 * Reset manifest structure.
 */

void bellatrix_plugin_manifest_clear(
    BellatrixPluginManifest *manifest
);

#ifdef __cplusplus
}
#endif

#endif