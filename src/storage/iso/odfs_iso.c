// src/storage/iso/odfs_iso.c
// Host-side ODFileSystem adapter for Bellatrix harness.

#ifdef BELLATRIX_HARNESS

#include "storage/iso/odfs_iso.h"

#include "odfs/api.h"
#include "odfs/log.h"
#include "odfs/node.h"
#include "odfs/media.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Declared in platform/host/file_media.c
odfs_err_t odfs_media_open_image(const char *path, odfs_media_t *out);

struct OdfsIso {
    odfs_media_t     media;
    odfs_mount_t     mount;
    odfs_log_state_t log;
    bool             media_open;
    bool             mounted;
};

OdfsIso *odfs_iso_open(const char *path)
{
    OdfsIso *iso;
    odfs_mount_opts_t opts;

    if (!path) return NULL;

    iso = calloc(1u, sizeof(*iso));
    if (!iso) return NULL;

    odfs_log_init(&iso->log);
    odfs_log_set_level(&iso->log, ODFS_LOG_WARN);

    if (odfs_media_open_image(path, &iso->media) != ODFS_OK) {
        free(iso);
        return NULL;
    }
    iso->media_open = true;

    odfs_mount_opts_default(&opts);
    opts.lowercase_iso = 0;

    if (odfs_mount(&iso->media, &opts, &iso->log, &iso->mount) != ODFS_OK) {
        iso->media.ops->close(iso->media.ctx);
        free(iso);
        return NULL;
    }
    iso->mounted = true;
    return iso;
}

void odfs_iso_close(OdfsIso *iso)
{
    if (!iso) return;
    if (iso->mounted)
        odfs_unmount(&iso->mount);
    if (iso->media_open && iso->media.ops && iso->media.ops->close)
        iso->media.ops->close(iso->media.ctx);
    free(iso);
}

uint32_t odfs_iso_sector_count(const OdfsIso *iso)
{
    if (!iso || !iso->media_open) return 0u;
    return iso->media.ops->sector_count(iso->media.ctx);
}

bool odfs_iso_read_sectors(OdfsIso *iso, uint32_t lba, uint32_t count,
                            void *buf)
{
    if (!iso || !iso->media_open || !buf) return false;
    return iso->media.ops->read_sectors(iso->media.ctx, lba, count, buf)
           == ODFS_OK;
}

bool odfs_iso_read_fn(void *ctx, uint32_t lba, uint32_t count, void *dst)
{
    return odfs_iso_read_sectors((OdfsIso *)ctx, lba, count, dst);
}

void *odfs_iso_resolve(OdfsIso *iso, const char *path)
{
    odfs_node_t *node;

    if (!iso || !iso->mounted || !path) return NULL;

    node = malloc(sizeof(*node));
    if (!node) return NULL;

    if (odfs_resolve_path(&iso->mount, path, node) != ODFS_OK) {
        free(node);
        return NULL;
    }
    return node;
}

ssize_t odfs_iso_read_node(OdfsIso *iso, void *node_opaque, void *buf,
                            size_t buf_size)
{
    odfs_node_t *node;
    size_t len;

    if (!iso || !iso->mounted || !node_opaque || !buf) return -1;

    node = (odfs_node_t *)node_opaque;
    if (node->kind != ODFS_NODE_FILE) return -1;

    len = buf_size;
    if (odfs_read(&iso->mount, node, 0u, buf, &len) != ODFS_OK)
        return -1;

    return (ssize_t)len;
}

ssize_t odfs_iso_read_file(OdfsIso *iso, const char *path, void *buf,
                            size_t buf_size)
{
    void *node;
    ssize_t ret;

    node = odfs_iso_resolve(iso, path);
    if (!node) return -1;

    ret = odfs_iso_read_node(iso, node, buf, buf_size);
    free(node);
    return ret;
}

const char *odfs_iso_volume_name(const OdfsIso *iso)
{
    if (!iso || !iso->mounted) return NULL;
    return iso->mount.volume_name;
}

#endif // BELLATRIX_HARNESS
