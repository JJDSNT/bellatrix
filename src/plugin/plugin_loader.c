#include "plugin/plugin_loader.h"

#include "plugin/plugin_abi.h"
#include "plugin/plugin_manifest.h"
#include "plugin/plugin_registry.h"

#include "core/expansion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <limits.h>
#endif

static int plugin_api_register_expansion(
    BellatrixMachine *machine,
    const BellatrixExpansionDesc *desc
)
{
    return bellatrix_expansion_register(machine, desc);
}

static BellatrixExpansion *plugin_api_find_expansion(
    BellatrixMachine *machine,
    const char *id
)
{
    return bellatrix_expansion_find(machine, id);
}

static void plugin_api_init(BellatrixPluginApi *api)
{
    memset(api, 0, sizeof(*api));

    api->abi_version = BELLATRIX_PLUGIN_ABI_VERSION;
    api->register_expansion = plugin_api_register_expansion;
    api->find_expansion = plugin_api_find_expansion;
}

int bellatrix_plugin_loader_init(
    BellatrixPluginLoader *loader,
    const char *search_path
)
{
    if (!loader || !search_path) {
        return -1;
    }

    memset(loader, 0, sizeof(*loader));

    loader->search_path = search_path;

    plugin_api_init(&loader->api);

    return 0;
}

void bellatrix_plugin_loader_shutdown(
    BellatrixPluginLoader *loader
)
{
    (void)loader;
}

static int load_plugin_module(
    BellatrixPluginLoader *loader,
    BellatrixMachine *machine,
    const BellatrixPluginManifest *manifest,
    const char *directory_path
)
{
    char module_path[512];

#if defined(_WIN32)
    HMODULE handle;
#else
    void *handle;
#endif

    BellatrixPluginInitFn init_fn;

    if (!loader || !machine || !manifest || !directory_path) {
        return -1;
    }

    snprintf(
        module_path,
        sizeof(module_path),
        "%s/%s",
        directory_path,
        manifest->module
    );

    loader->api.plugin_root = directory_path;

#if defined(_WIN32)

    handle = LoadLibraryA(module_path);
    if (!handle) {
        return -2;
    }

    init_fn = (BellatrixPluginInitFn)GetProcAddress(
        handle,
        BELLATRIX_PLUGIN_ENTRY_SYMBOL
    );

#else

    handle = dlopen(module_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "[PLUGIN] dlopen failed: %s\n", dlerror());
        return -2;
    }

    init_fn = (BellatrixPluginInitFn)dlsym(
        handle,
        BELLATRIX_PLUGIN_ENTRY_SYMBOL
    );

#endif

    if (!init_fn) {
        fprintf(stderr,
            "[PLUGIN] missing entry symbol: %s\n",
            BELLATRIX_PLUGIN_ENTRY_SYMBOL);

        return -3;
    }

    return init_fn(machine, &loader->api);
}

int bellatrix_plugin_load_directory(
    BellatrixPluginLoader *loader,
    BellatrixMachine *machine,
    const char *path
)
{
    BellatrixPluginManifest manifest;
    char manifest_path[512];

    if (!loader || !machine || !path) {
        return -1;
    }

    memset(&manifest, 0, sizeof(manifest));

    snprintf(
        manifest_path,
        sizeof(manifest_path),
        "%s/plugin.json",
        path
    );

    if (bellatrix_plugin_manifest_load(
            manifest_path,
            &manifest) != 0) {

        fprintf(stderr,
            "[PLUGIN] failed to load manifest: %s\n",
            manifest_path);

        return -2;
    }

    if (manifest.abi_version != BELLATRIX_PLUGIN_ABI_VERSION) {
        fprintf(stderr,
            "[PLUGIN] ABI mismatch for %s\n",
            manifest.id);

        return -3;
    }

    return load_plugin_module(
        loader,
        machine,
        &manifest,
        path
    );
}

int bellatrix_plugin_load_all(
    BellatrixPluginLoader *loader,
    BellatrixMachine *machine
)
{
#if defined(_WIN32)

    (void)loader;
    (void)machine;

    return -1;

#else

    DIR *dir;
    struct dirent *entry;

    if (!loader || !machine || !loader->search_path) {
        return -1;
    }

    dir = opendir(loader->search_path);
    if (!dir) {
        fprintf(stderr,
            "[PLUGIN] unable to open directory: %s\n",
            loader->search_path);

        return -2;
    }

    while ((entry = readdir(dir)) != NULL) {

        char plugin_dir[PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(
            plugin_dir,
            sizeof(plugin_dir),
            "%s/%s",
            loader->search_path,
            entry->d_name
        );

        bellatrix_plugin_load_directory(
            loader,
            machine,
            plugin_dir
        );
    }

    closedir(dir);

    return 0;

#endif
}
