#include "plugin/plugin_manifest.h"

#include "plugin/plugin_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int extract_json_string(
    const char *json,
    const char *key,
    char *out,
    size_t out_size
)
{
    char pattern[128];
    const char *p;
    const char *start;
    const char *end;
    size_t len;

    if (!json || !key || !out || out_size == 0) {
        return -1;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    p = strstr(json, pattern);
    if (!p) {
        return -2;
    }

    p = strchr(p, ':');
    if (!p) {
        return -3;
    }

    start = strchr(p, '"');
    if (!start) {
        return -4;
    }

    start++;

    end = strchr(start, '"');
    if (!end) {
        return -5;
    }

    len = (size_t)(end - start);

    if (len >= out_size) {
        len = out_size - 1;
    }

    memcpy(out, start, len);
    out[len] = '\0';

    return 0;
}

static int extract_json_u32(
    const char *json,
    const char *key,
    uint32_t *value
)
{
    char pattern[128];
    const char *p;

    if (!json || !key || !value) {
        return -1;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    p = strstr(json, pattern);
    if (!p) {
        return -2;
    }

    p = strchr(p, ':');
    if (!p) {
        return -3;
    }

    p++;

    while (*p == ' ' || *p == '\t') {
        p++;
    }

    *value = (uint32_t)strtoul(p, NULL, 10);

    return 0;
}

void bellatrix_plugin_manifest_clear(
    BellatrixPluginManifest *manifest
)
{
    if (!manifest) {
        return;
    }

    memset(manifest, 0, sizeof(*manifest));
}

int bellatrix_plugin_manifest_load(
    const char *path,
    BellatrixPluginManifest *manifest
)
{
    FILE *fp;
    long size;
    char *json;
    size_t read_size;

    if (!path || !manifest) {
        return -1;
    }

    bellatrix_plugin_manifest_clear(manifest);

    fp = fopen(path, "rb");
    if (!fp) {
        return -2;
    }

    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return -3;
    }

    json = (char *)calloc(1, (size_t)size + 1);
    if (!json) {
        fclose(fp);
        return -4;
    }

    read_size = fread(json, 1, (size_t)size, fp);

    fclose(fp);

    if (read_size != (size_t)size) {
        free(json);
        return -5;
    }

    extract_json_string(
        json,
        "id",
        manifest->id,
        sizeof(manifest->id)
    );

    extract_json_string(
        json,
        "name",
        manifest->name,
        sizeof(manifest->name)
    );

    extract_json_string(
        json,
        "version",
        manifest->version,
        sizeof(manifest->version)
    );

    extract_json_string(
        json,
        "author",
        manifest->author,
        sizeof(manifest->author)
    );

    extract_json_string(
        json,
        "module",
        manifest->module,
        sizeof(manifest->module)
    );

    extract_json_u32(
        json,
        "priority",
        &manifest->priority
    );

    extract_json_u32(
        json,
        "abi_version",
        &manifest->abi_version
    );

    if (manifest->abi_version == 0) {
        manifest->abi_version =
            BELLATRIX_PLUGIN_ABI_VERSION;
    }

    /*
     * Temporary simplistic kind parsing.
     */

    if (strstr(json, "\"kind\":\"board\"")) {
        manifest->kind = BELLATRIX_PLUGIN_BOARD;
    }
    else if (strstr(json, "\"kind\":\"device\"")) {
        manifest->kind = BELLATRIX_PLUGIN_DEVICE;
    }
    else {
        manifest->kind = BELLATRIX_PLUGIN_SERVICE;
    }

    free(json);

    return 0;
}