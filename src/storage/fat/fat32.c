// src/storage/fat/fat32.c
// Minimal FAT32 reader.  Read-only, single partition, VFAT LFN aware.

#include "storage/fat/fat32.h"
#include "storage/fat/fat32_lfn.h"
#include <string.h>

int kprintf(const char *fmt, ...);

// ---------------------------------------------------------------------------
// Little-endian accessors (sector buffers are byte arrays)
// ---------------------------------------------------------------------------

static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

// ---------------------------------------------------------------------------
// Sector buffer (shared scratch)
// ---------------------------------------------------------------------------

// 32-byte alignment required for USB DMA
static uint8_t s_sector[512] __attribute__((aligned(32)));

static bool read_sector(const Fat32State *fs, uint32_t lba)
{
    return fs->read_block(fs->read_ctx, lba, s_sector);
}

// ---------------------------------------------------------------------------
// FAT32 cluster helpers
// ---------------------------------------------------------------------------

// Follow FAT chain: returns next cluster (0x0FFFFFFF = end of chain, 0 = error)
static uint32_t fat_next_cluster(const Fat32State *fs, uint32_t cluster)
{
    // Each FAT32 entry is 4 bytes (sector holds 128 entries per 512-byte sector)
    uint32_t fat_sector  = fs->fat_lba + (cluster / 128u);
    uint32_t fat_offset  = (cluster % 128u) * 4u;

    if (!read_sector(fs, fat_sector)) {
        return 0u;
    }

    return le32(s_sector + fat_offset) & 0x0FFFFFFFu;
}

static uint32_t cluster_to_lba(const Fat32State *fs, uint32_t cluster)
{
    return fs->data_lba + (cluster - 2u) * fs->cluster_size;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool fat32_init_with_reader(Fat32State *fs, Fat32ReadBlockFn read_fn, void *ctx)
{
    if (!fs || !read_fn) return false;
    memset(fs, 0, sizeof(*fs));
    fs->read_block = read_fn;
    fs->read_ctx   = ctx;

    // Read MBR (sector 0)
    if (!read_sector(fs, 0u)) {
        kprintf("[FAT32] cannot read MBR\n");
        return false;
    }

    // MBR signature
    if (s_sector[510] != 0x55u || s_sector[511] != 0xAAu) {
        kprintf("[FAT32] bad MBR signature\n");
        return false;
    }

    // Scan partition table (4 entries starting at 0x1BE).  Accept any
    // FAT-ish partition type — Raspberry Pi boot partitions show up as
    // 0x0B/0x0C (FAT32) but also 0x0E/0x06/0x04 (FAT16 ids, sometimes used
    // for FAT32 volumes by formatters); the FAT32 BPB check below decides.
    uint32_t part_lba = 0u;
    for (unsigned i = 0; i < 4u; i++) {
        const uint8_t *entry = s_sector + 0x1BE + i * 16u;
        uint8_t  type = entry[4];
        uint32_t lba  = le32(entry + 8);

        if (type != 0u)
            kprintf("[FAT32] MBR part %u: type=0x%02x lba=%u size=%u\n",
                    i, (unsigned)type, (unsigned)lba,
                    (unsigned)le32(entry + 12));

        if (part_lba == 0u && lba != 0u &&
            (type == 0x0Bu || type == 0x0Cu ||
             type == 0x0Eu || type == 0x06u || type == 0x04u)) {
            part_lba = lba;
        }
    }

    // No MBR partition? Some cards are formatted as a superfloppy
    // (filesystem starts at sector 0); try the BPB right there.
    if (part_lba == 0u) {
        kprintf("[FAT32] no FAT partition in MBR, trying superfloppy\n");
    }

    // Read FAT32 boot sector (BPB)
    if (!read_sector(fs, part_lba)) {
        kprintf("[FAT32] cannot read BPB at LBA %u\n", (unsigned)part_lba);
        return false;
    }

    // BPB fields
    uint16_t bytes_per_sec = le16(s_sector + 11);
    uint8_t  sec_per_clust = s_sector[13];
    uint16_t reserved_sec  = le16(s_sector + 14);
    uint8_t  num_fats      = s_sector[16];
    uint16_t fat16_size    = le16(s_sector + 22);
    uint32_t fat32_size    = le32(s_sector + 36);
    uint32_t root_cluster  = le32(s_sector + 44);

    if (bytes_per_sec != 512u) {
        kprintf("[FAT32] unsupported sector size %u\n", (unsigned)bytes_per_sec);
        return false;
    }

    // FAT32 has fat_size16 == 0 and a real fat_size32; FAT12/16 volumes
    // (small boot partitions) are not supported by this reader.
    if (fat16_size != 0u || fat32_size == 0u || root_cluster < 2u) {
        kprintf("[FAT32] volume at LBA %u is not FAT32 (fat16_size=%u) — "
                "reformat the partition as FAT32 to use it\n",
                (unsigned)part_lba, (unsigned)fat16_size);
        return false;
    }

    fs->part_lba         = part_lba;
    fs->bytes_per_sector = bytes_per_sec;
    fs->cluster_size     = sec_per_clust;
    fs->fat_lba          = part_lba + reserved_sec;
    fs->fat_size         = fat32_size;
    fs->data_lba         = fs->fat_lba + (uint32_t)num_fats * fat32_size;
    fs->root_cluster     = root_cluster;
    fs->initialized      = true;

    kprintf("[FAT32] init OK: part_lba=%u clust_sec=%u root_clust=%u\n",
            (unsigned)part_lba, (unsigned)sec_per_clust, (unsigned)root_cluster);
    return true;
}

// ---------------------------------------------------------------------------
// Name helpers
// ---------------------------------------------------------------------------

// Copy a UTF-8 string truncated to cap bytes (incl. NUL) without splitting
// a multi-byte sequence.
static void utf8_copy_truncated(char *dst, const char *src, size_t cap)
{
    size_t n = 0u;

    while (src[n]) {
        unsigned char lead = (unsigned char)src[n];
        size_t seq = 1u;
        if      ((lead & 0xE0u) == 0xC0u) seq = 2u;
        else if ((lead & 0xF0u) == 0xE0u) seq = 3u;
        else if ((lead & 0xF8u) == 0xF0u) seq = 4u;

        if (n + seq + 1u > cap) break;
        for (size_t i = 0u; i < seq && src[n]; i++) {
            dst[n] = src[n];
            n++;
        }
    }

    dst[n] = '\0';
}

// Case-insensitive compare for the ASCII range; non-ASCII bytes must match
// exactly (FAT LFN matching is case-insensitive; full Unicode folding is
// not worth carrying on bare metal).
static bool name_equal_icase(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == *b;
}

// ---------------------------------------------------------------------------
// Directory scan (LFN-aware)
// ---------------------------------------------------------------------------

// Process one directory entry; name is UTF-8 (LFN if present, 8.3 fallback).
// Return false to stop the scan.
typedef bool (*dir_entry_fn)(const uint8_t *entry, const char *name, void *user);

static bool scan_dir_cluster(const Fat32State *fs, uint32_t cluster,
                              dir_entry_fn fn, void *user)
{
    // Static: the LFN accumulator + UTF-8 scratch are ~1.5 KB — keep them
    // off the bare-metal stack.  Scanning is single-threaded (Core 0).
    static fat32_lfn_state_t s_lfn;
    static char              s_lfn_utf8[FAT32_LFN_MAX_UTF8];

    fat32_lfn_reset(&s_lfn);

    for (;;) {
        uint32_t lba = cluster_to_lba(fs, cluster);

        for (uint32_t sec = 0u; sec < fs->cluster_size; sec++) {
            if (!read_sector(fs, lba + sec)) {
                return false;
            }

            for (unsigned i = 0u; i < 512u / 32u; i++) {
                const uint8_t *entry = s_sector + i * 32u;

                if (entry[0] == 0x00u) return true;   // end of directory
                if (entry[0] == 0xE5u) {              // deleted
                    fat32_lfn_reset(&s_lfn);
                    continue;
                }
                if ((entry[11] & 0x3Fu) == FAT32_ATTR_LFN) {
                    fat32_lfn_push_entry(&s_lfn, entry);
                    continue;
                }
                if (entry[11] & 0x08u) {              // volume label
                    fat32_lfn_reset(&s_lfn);
                    continue;
                }
                if (entry[11] & 0x10u) {              // subdirectory
                    fat32_lfn_reset(&s_lfn);
                    continue;
                }

                char name[FAT32_NAME_MAX];
                if (fat32_lfn_finish(&s_lfn, entry, s_lfn_utf8, sizeof(s_lfn_utf8)) == 0) {
                    utf8_copy_truncated(name, s_lfn_utf8, sizeof(name));
                } else {
                    fat32_short_name_to_string(entry, name, sizeof(name));
                }
                // finish() only resets on success — clear leftovers from
                // orphaned/corrupt LFN chains so they can't leak forward
                fat32_lfn_reset(&s_lfn);

                if (!fn(entry, name, user)) return false;
            }
        }

        uint32_t next = fat_next_cluster(fs, cluster);
        if (next >= 0x0FFFFFF8u || next == 0u) break;
        cluster = next;
    }
    return true;
}

// ---------------------------------------------------------------------------
// List files
// ---------------------------------------------------------------------------

typedef struct {
    char     (*names)[FAT32_NAME_MAX];
    uint32_t  max;
    uint32_t  count;
} ListCtx;

static bool list_entry_fn(const uint8_t *entry, const char *name, void *user)
{
    ListCtx *ctx = (ListCtx *)user;
    (void)entry;

    if (ctx->count >= ctx->max) return false;

    utf8_copy_truncated(ctx->names[ctx->count], name, FAT32_NAME_MAX);
    ctx->count++;
    return true;
}

uint32_t fat32_list(Fat32State *fs,
                    char        names[][FAT32_NAME_MAX],
                    uint32_t    max_count)
{
    if (!fs || !fs->initialized || !names || max_count == 0u) return 0u;

    ListCtx ctx = { names, max_count, 0u };
    scan_dir_cluster(fs, fs->root_cluster, list_entry_fn, &ctx);
    return ctx.count;
}

// ---------------------------------------------------------------------------
// Open file
// ---------------------------------------------------------------------------

typedef struct {
    const char *target;
    bool        found;
    uint32_t    start_cluster;
    uint32_t    file_size;
} OpenCtx;

static bool open_entry_fn(const uint8_t *entry, const char *name, void *user)
{
    OpenCtx *ctx = (OpenCtx *)user;
    bool match = name_equal_icase(name, ctx->target);

    if (!match) {
        // Also accept the 8.3 form when the listing produced an LFN
        char short83[13];
        fat32_short_name_to_string(entry, short83, sizeof(short83));
        match = name_equal_icase(short83, ctx->target);
    }

    if (!match) return true;

    ctx->start_cluster = ((uint32_t)le16(entry + 20) << 16) | le16(entry + 26);
    ctx->file_size     = le32(entry + 28);
    ctx->found         = true;
    return false;   // stop scanning
}

bool fat32_open(Fat32State *fs, const char *name, Fat32File *out)
{
    if (!fs || !fs->initialized || !name || !out) return false;

    OpenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.target = name;

    scan_dir_cluster(fs, fs->root_cluster, open_entry_fn, &ctx);

    if (!ctx.found) {
        kprintf("[FAT32] file not found: %s\n", name);
        return false;
    }

    out->fs           = fs;
    out->start_cluster = ctx.start_cluster;
    out->file_size    = ctx.file_size;
    out->cur_cluster  = ctx.start_cluster;
    out->cur_offset   = 0u;
    return true;
}

// ---------------------------------------------------------------------------
// File read
// ---------------------------------------------------------------------------

uint32_t fat32_read(Fat32File *f, void *buf, uint32_t len)
{
    if (!f || !buf || len == 0u) return 0u;
    if (f->cur_offset >= f->file_size) return 0u;

    uint32_t remaining = f->file_size - f->cur_offset;
    if (len > remaining) len = remaining;

    const Fat32State *fs          = f->fs;
    uint32_t          cluster_bytes = fs->cluster_size * 512u;
    uint32_t          done          = 0u;
    uint8_t          *dst           = (uint8_t *)buf;

    while (done < len) {
        uint32_t cluster_off = f->cur_offset % cluster_bytes;
        uint32_t sec_in_cluster = cluster_off / 512u;
        uint32_t byte_in_sec    = cluster_off % 512u;

        uint32_t lba = cluster_to_lba(fs, f->cur_cluster) + sec_in_cluster;
        if (!read_sector(f->fs, lba)) break;

        uint32_t can_copy = 512u - byte_in_sec;
        uint32_t to_copy  = len - done;
        if (to_copy > can_copy) to_copy = can_copy;

        memcpy(dst + done, s_sector + byte_in_sec, to_copy);
        done             += to_copy;
        f->cur_offset    += to_copy;

        // Advance cluster if we crossed a cluster boundary
        if (f->cur_offset > 0u &&
            (f->cur_offset % cluster_bytes) == 0u) {
            uint32_t next = fat_next_cluster(fs, f->cur_cluster);
            if (next >= 0x0FFFFFF8u || next == 0u) break;
            f->cur_cluster = next;
        }
    }

    return done;
}

bool fat32_read_all(Fat32File *f, void *buf, uint32_t buf_size)
{
    if (!f || !buf) return false;
    if (buf_size < f->file_size) return false;

    // Reset to beginning
    f->cur_cluster = f->start_cluster;
    f->cur_offset  = 0u;

    uint32_t got = fat32_read(f, buf, f->file_size);
    return (got == f->file_size);
}

bool fat32_seek(Fat32File *f, uint32_t byte_offset)
{
    if (!f || !f->fs) return false;
    if (byte_offset >= f->file_size) return false;

    const Fat32State *fs          = f->fs;
    uint32_t          cluster_bytes = fs->cluster_size * 512u;
    uint32_t          target_idx    = byte_offset / cluster_bytes;
    uint32_t          cur_idx       = f->cur_offset / cluster_bytes;

    uint32_t cluster;
    uint32_t idx;

    if (target_idx >= cur_idx) {
        // Seek forward from current cluster
        cluster = f->cur_cluster;
        idx     = cur_idx;
    } else {
        // Seek backward: rewind to start
        cluster = f->start_cluster;
        idx     = 0u;
    }

    while (idx < target_idx) {
        uint32_t next = fat_next_cluster(fs, cluster);
        if (next >= 0x0FFFFFF8u || next == 0u) return false;
        cluster = next;
        idx++;
    }

    f->cur_cluster = cluster;
    f->cur_offset  = byte_offset;
    return true;
}

// ---------------------------------------------------------------------------
// In-place overwrite (conservative write support)
// ---------------------------------------------------------------------------

void fat32_set_writer(Fat32State *fs, Fat32WriteBlockFn write_fn, void *ctx)
{
    if (!fs) return;
    fs->write_block = write_fn;
    fs->write_ctx   = ctx;
}

// Separate buffer: s_sector is clobbered by fat_next_cluster() between
// sector writes (it reads the FAT through the shared buffer).
static uint8_t s_wsector[512] __attribute__((aligned(32)));

uint32_t fat32_overwrite_in_place(Fat32State *fs, const char *name,
                                  const void *data, uint32_t len)
{
    Fat32File f;
    const uint8_t *src = (const uint8_t *)data;

    if (!fs || !fs->initialized || !fs->write_block || !data) return 0u;
    if (!fat32_open(fs, name, &f)) {
        kprintf("[FAT32] overwrite: '%s' not found\n", name);
        return 0u;
    }
    if (f.file_size == 0u || f.start_cluster < 2u) return 0u;

    if (len > f.file_size) {
        kprintf("[FAT32] overwrite: '%s' truncating %u -> %u bytes\n",
                name, (unsigned)len, (unsigned)f.file_size);
        len = f.file_size;
    }

    uint32_t cluster = f.start_cluster;
    uint32_t pos     = 0u;   // byte offset within the file

    while (pos < f.file_size && cluster >= 2u && cluster < 0x0FFFFFF8u) {
        uint32_t lba = cluster_to_lba(fs, cluster);

        for (uint32_t s = 0u; s < fs->cluster_size && pos < f.file_size; s++) {
            for (uint32_t i = 0u; i < 512u; i++) {
                uint32_t off = pos + i;
                s_wsector[i] = (off < len) ? src[off] : (uint8_t)' ';
            }
            if (!fs->write_block(fs->write_ctx, lba + s, s_wsector)) {
                kprintf("[FAT32] overwrite: write error lba=%u\n",
                        (unsigned)(lba + s));
                return 0u;
            }
            pos += 512u;
        }

        cluster = fat_next_cluster(fs, cluster);
        if (cluster == 0u) return 0u;
    }

    return len;
}
