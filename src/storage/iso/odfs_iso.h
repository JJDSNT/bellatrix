// src/storage/iso/odfs_iso.h
// Host-side ISO reader backed by ODFileSystem (POSIX / harness only).
//
// Provides two layers:
//  1. Raw sector access — compatible with iso_read_fn for ATAPI emulation.
//  2. Filesystem access — resolve paths, read files from the ISO.
//
// Bare-metal: use iso_image.h with a custom odfs_media_t instead.

#pragma once
#ifdef BELLATRIX_HARNESS

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Opaque handle to an open ISO image.
typedef struct OdfsIso OdfsIso;

// Open an ISO image or .cue file and parse its filesystem.
// Returns NULL on failure (unrecognized format, I/O error, etc.).
OdfsIso *odfs_iso_open(const char *path);

// Release all resources.
void odfs_iso_close(OdfsIso *iso);

// Total number of 2048-byte sectors in the image.
uint32_t odfs_iso_sector_count(const OdfsIso *iso);

// Read `count` sectors starting at `lba` into `buf`.
// `buf` must hold at least count * 2048 bytes.
bool odfs_iso_read_sectors(OdfsIso *iso, uint32_t lba, uint32_t count,
                            void *buf);

// iso_read_fn-compatible callback for bellatrix_machine_attach_iso_fn().
// Pass an OdfsIso * as ctx.
bool odfs_iso_read_fn(void *ctx, uint32_t lba, uint32_t count, void *dst);

// Resolve a POSIX-style absolute path (e.g. "/s/startup-sequence").
// Returns a heap-allocated odfs_node_t on success; caller must free() it.
// Returns NULL if the path does not exist.
void *odfs_iso_resolve(OdfsIso *iso, const char *path);

// Read the full content of a resolved file node into buf.
// Returns bytes read, or -1 on error (node is not a file, buf too small, etc.).
ssize_t odfs_iso_read_node(OdfsIso *iso, void *node, void *buf,
                            size_t buf_size);

// Convenience: resolve path + read entire file in one call.
// Returns bytes read, or -1 on error.
ssize_t odfs_iso_read_file(OdfsIso *iso, const char *path, void *buf,
                            size_t buf_size);

// Returns the volume name string from the ISO (e.g. "AROS" or "Workbench").
// Points into iso's internal state; valid until odfs_iso_close().
const char *odfs_iso_volume_name(const OdfsIso *iso);

#endif // BELLATRIX_HARNESS
