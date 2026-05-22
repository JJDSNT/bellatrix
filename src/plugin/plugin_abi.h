#ifndef BELLATRIX_PLUGIN_ABI_H
#define BELLATRIX_PLUGIN_ABI_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Plugin ABI version.
 *
 * Increment this whenever the Bellatrix plugin API changes
 * in a binary incompatible way.
 */

#define BELLATRIX_PLUGIN_ABI_VERSION 1

/*
 * Required exported symbol name.
 *
 * Every expansion plugin must expose:
 *
 *     int bellatrix_plugin_init(
 *         BellatrixMachine *machine,
 *         const BellatrixPluginApi *api
 *     );
 */

#define BELLATRIX_PLUGIN_ENTRY_SYMBOL "bellatrix_plugin_init"

#ifdef __cplusplus
}
#endif

#endif