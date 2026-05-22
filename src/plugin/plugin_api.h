#ifndef BELLATRIX_PLUGIN_API_H
#define BELLATRIX_PLUGIN_API_H

#include "core/expansion.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BellatrixPluginApi {

    /*
     * ABI version supported by the host.
     */

    uint32_t abi_version;

    /*
     * Register a machine expansion.
     *
     * The expansion becomes owned by the machine after
     * successful registration.
     */

    int (*register_expansion)(
        BellatrixMachine *machine,
        const BellatrixExpansionDesc *desc
    );

    /*
     * Lookup expansion by id.
     */

    BellatrixExpansion *(*find_expansion)(
        BellatrixMachine *machine,
        const char *id
    );

} BellatrixPluginApi;

#ifdef __cplusplus
}
#endif

#endif