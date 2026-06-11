// src/storage/fat/fat32.h
// Minimal FAT32 reader for bare-metal use.
// Supports: MBR, FAT32 BPB, root directory scan, sequential file read,
// VFAT long file names (UTF-16 → UTF-8).
// Read-only.

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// UTF-8 display name capacity (LFN; longer names are truncated on a
// character boundary).  Listing and open use the same truncated form,
// so a name returned by fat32_list always opens.
#define FAT32_NAME_MAX  128u

// Block reader callback: read one 512-byte sector at LBA into buf.
typedef bool (*Fat32ReadBlockFn)(void *ctx, uint32_t lba, uint8_t *buf);

// Block writer callback: write one 512-byte sector at LBA from buf.
typedef bool (*Fat32WriteBlockFn)(void *ctx, uint32_t lba, const uint8_t *buf);

typedef struct Fat32State {
    uint32_t part_lba;          // partition start LBA
    uint32_t cluster_size;      // sectors per cluster
    uint32_t fat_lba;           // first FAT start LBA
    uint32_t data_lba;          // first data sector LBA
    uint32_t root_cluster;      // root directory first cluster
    uint32_t fat_size;          // sectors per FAT
    uint16_t bytes_per_sector;  // usually 512
    bool     initialized;
    Fat32ReadBlockFn read_block; // pluggable block reader
    void            *read_ctx;  // opaque context for read_block
    Fat32WriteBlockFn write_block; // optional block writer (NULL = read-only)
    void             *write_ctx;   // opaque context for write_block
} Fat32State;

typedef struct Fat32File {
    Fat32State *fs;
    uint32_t   start_cluster;
    uint32_t   file_size;
    // current position bookkeeping
    uint32_t   cur_cluster;
    uint32_t   cur_offset;      // byte offset from start
} Fat32File;

// Initialise with a custom block reader (e.g. USB MSC)
bool fat32_init_with_reader(Fat32State *fs, Fat32ReadBlockFn read_fn, void *ctx);

// List every regular file in the root directory (no extension filter —
// callers decide what is interesting).  Names are UTF-8, LFN when present,
// 8.3 fallback otherwise.
// names: array of FAT32_NAME_MAX-byte strings; max_count: capacity.
// Returns number of entries filled.
uint32_t fat32_list(Fat32State *fs,
                    char       names[][FAT32_NAME_MAX],
                    uint32_t   max_count);

// Open a root-directory file by name as returned by fat32_list (long or
// 8.3 form; ASCII characters compare case-insensitively).
bool fat32_open(Fat32State *fs, const char *name, Fat32File *out);

// Read up to len bytes from the file into buf; returns bytes read.
uint32_t fat32_read(Fat32File *f, void *buf, uint32_t len);

// Convenience: read entire file into buf (must be >= f->file_size bytes).
bool fat32_read_all(Fat32File *f, void *buf, uint32_t buf_size);

// Seek to byte_offset within the file; updates cur_cluster accordingly.
// Returns false if byte_offset >= file_size or FAT chain is broken.
bool fat32_seek(Fat32File *f, uint32_t byte_offset);

// Attach a block writer (e.g. SD CMD24).  Required before overwrite.
void fat32_set_writer(Fat32State *fs, Fat32WriteBlockFn write_fn, void *ctx);

// Overwrite the DATA of an existing root-directory file in place.
// Deliberately conservative: never touches the FAT or directory entries
// (no allocation, no size change, no corruption risk).  Data beyond len is
// padded with spaces up to the existing file size; len is clamped to the
// file size.  Returns bytes of payload actually written, 0 on failure
// (file missing, no writer attached, or I/O error).
uint32_t fat32_overwrite_in_place(Fat32State *fs, const char *name,
                                  const void *data, uint32_t len);
