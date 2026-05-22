#ifndef BELLATRIX_PLUGIN_TYPES_H
#define BELLATRIX_PLUGIN_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BellatrixMachine BellatrixMachine;
typedef struct BellatrixExpansion BellatrixExpansion;
typedef struct BellatrixExpansionDesc BellatrixExpansionDesc;
typedef struct BellatrixPluginApi BellatrixPluginApi;

/*
 * Generic plugin descriptor.
 *
 * Represents a loaded expansion module.
 */

typedef struct BellatrixPlugin {
    const char *id;
    const char *name;
    const char *version;
    const char *author;

    uint32_t abi_version;

    void *dl_handle;
    void *userdata;
} BellatrixPlugin;

/*
 * Expansion categories.
 */

typedef enum BellatrixPluginKind {
    BELLATRIX_PLUGIN_BOARD = 1,
    BELLATRIX_PLUGIN_DEVICE,
    BELLATRIX_PLUGIN_SERVICE
} BellatrixPluginKind;

/*
 * Plugin manifest data loaded from plugin.json.
 */

typedef struct BellatrixPluginManifest {
    char id[64];
    char name[64];
    char version[32];
    char author[64];

    char module[256];

    uint32_t abi_version;
    uint32_t priority;

    BellatrixPluginKind kind;
} BellatrixPluginManifest;

/*
 * Required plugin entrypoint signature.
 */

typedef int (*BellatrixPluginInitFn)(
    BellatrixMachine *machine,
    const BellatrixPluginApi *api
);

#ifdef __cplusplus
}
#endif

#endif