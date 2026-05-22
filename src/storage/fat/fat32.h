// src/storage/fat/fat32.h
// Minimal FAT32 reader for bare-metal use.
// Supports: MBR, FAT32 BPB, root directory scan, sequential file read.
// Read-only.  No LFN support (8.3 names only).

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define FAT32_NAME_MAX  12u   // "FILENAME.EXT" + NUL

typedef struct Fat32State {
    uint32_t part_lba;          // partition start LBA
    uint32_t cluster_size;      // sectors per cluster
    uint32_t fat_lba;           // first FAT start LBA
    uint32_t data_lba;          // first data sector LBA
    uint32_t root_cluster;      // root directory first cluster
    uint32_t fat_size;          // sectors per FAT
    uint16_t bytes_per_sector;  // usually 512
    bool     initialized;
} Fat32State;

typedef struct Fat32File {
    Fat32State *fs;
    uint32_t   start_cluster;
    uint32_t   file_size;
    // current position bookkeeping
    uint32_t   cur_cluster;
    uint32_t   cur_offset;      // byte offset from start
} Fat32File;

// Initialise from the SD card; returns false if no FAT32 partition found
bool fat32_init(Fat32State *fs);

// List .ADF files in the root directory.
// names: array of FAT32_NAME_MAX-byte strings; max_count: capacity.
// Returns number of entries filled.
uint32_t fat32_list_adf(Fat32State *fs,
                        char       names[][FAT32_NAME_MAX],
                        uint32_t   max_count);

// Open a file by 8.3 uppercase name (e.g. "GAME    ADF").
// name_dot: regular dotted filename string, e.g. "GAME.ADF"
bool fat32_open(Fat32State *fs, const char *name_dot, Fat32File *out);

// Read up to len bytes from the file into buf; returns bytes read.
uint32_t fat32_read(Fat32File *f, void *buf, uint32_t len);

// Convenience: read entire file into buf (must be >= f->file_size bytes).
bool fat32_read_all(Fat32File *f, void *buf, uint32_t buf_size);

// Seek to byte_offset within the file; updates cur_cluster accordingly.
// Returns false if byte_offset >= file_size or FAT chain is broken.
bool fat32_seek(Fat32File *f, uint32_t byte_offset);

// List .ISO files in the root directory (same semantics as fat32_list_adf).
uint32_t fat32_list_iso(Fat32State *fs,
                        char       names[][FAT32_NAME_MAX],
                        uint32_t   max_count);
