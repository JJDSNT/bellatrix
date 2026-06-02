// src/storage/fat/fat32.c
// Minimal FAT32 reader.  Read-only, no LFN, single partition.

#include "storage/fat/fat32.h"
#include "storage/sdcard/bcm_emmc.h"
#include <string.h>

int kprintf(const char *fmt, ...);

// SD card block reader used by fat32_init()
static bool sd_read_block(void *ctx, uint32_t lba, uint8_t *buf)
{
    (void)ctx;
    return bcm_emmc_read_block(lba, buf);
}

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

    // Scan partition table (4 entries starting at 0x1BE)
    uint32_t part_lba = 0u;
    for (unsigned i = 0; i < 4u; i++) {
        const uint8_t *entry = s_sector + 0x1BE + i * 16u;
        uint8_t  type = entry[4];
        uint32_t lba  = le32(entry + 8);

        if ((type == 0x0Bu || type == 0x0Cu) && lba != 0u) {
            part_lba = lba;
            break;
        }
    }

    if (part_lba == 0u) {
        kprintf("[FAT32] no FAT32 partition found in MBR\n");
        return false;
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
    uint32_t fat32_size    = le32(s_sector + 36);
    uint32_t root_cluster  = le32(s_sector + 44);

    if (bytes_per_sec != 512u) {
        kprintf("[FAT32] unsupported sector size %u\n", (unsigned)bytes_per_sec);
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

bool fat32_init(Fat32State *fs)
{
    return fat32_init_with_reader(fs, sd_read_block, NULL);
}

// ---------------------------------------------------------------------------
// 8.3 name helpers
// ---------------------------------------------------------------------------

// Convert dotted name ("GAME.ADF") to FAT 8.3 padded uppercase (11 bytes, no dot)
static void to_83(const char *name_dot, char out[11])
{
    memset(out, ' ', 11);
    unsigned i = 0u, o = 0u;

    while (name_dot[i] && name_dot[i] != '.' && o < 8u) {
        char c = name_dot[i++];
        if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
        out[o++] = c;
    }

    if (name_dot[i] == '.') {
        i++;
        unsigned e = 0u;
        while (name_dot[i] && e < 3u) {
            char c = name_dot[i++];
            if (c >= 'a' && c <= 'z') c = (char)(c - ('a' - 'A'));
            out[8u + e++] = c;
        }
    }
}

// Convert FAT 8.3 (11 bytes, no dot) to dotted string; returns length
static unsigned from_83(const char raw[11], char out[FAT32_NAME_MAX])
{
    unsigned n = 0u;

    // Base name (8 bytes)
    for (unsigned i = 0u; i < 8u; i++) {
        if (raw[i] == ' ') break;
        out[n++] = raw[i];
    }

    // Extension (3 bytes)
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (unsigned i = 8u; i < 11u; i++) {
            if (raw[i] == ' ') break;
            out[n++] = raw[i];
        }
    }

    out[n] = '\0';
    return n;
}

// ---------------------------------------------------------------------------
// Directory scan
// ---------------------------------------------------------------------------

// Process one cluster's directory entries; returns false to stop
typedef bool (*dir_entry_fn)(const uint8_t *entry, void *user);

static bool scan_dir_cluster(const Fat32State *fs, uint32_t cluster,
                              dir_entry_fn fn, void *user)
{
    for (;;) {
        uint32_t lba = cluster_to_lba(fs, cluster);

        for (uint32_t sec = 0u; sec < fs->cluster_size; sec++) {
            if (!read_sector(fs, lba + sec)) {
                return false;
            }

            for (unsigned i = 0u; i < 512u / 32u; i++) {
                const uint8_t *entry = s_sector + i * 32u;

                if (entry[0] == 0x00u) return true;   // end of directory
                if (entry[0] == 0xE5u) continue;      // deleted
                if (entry[11] & 0x08u) continue;      // volume label
                if (entry[11] & 0x10u) continue;      // subdirectory
                if (entry[11] == 0x0Fu) continue;      // LFN entry

                if (!fn(entry, user)) return false;
            }
        }

        uint32_t next = fat_next_cluster(fs, cluster);
        if (next >= 0x0FFFFFF8u || next == 0u) break;
        cluster = next;
    }
    return true;
}

// ---------------------------------------------------------------------------
// List ADF files
// ---------------------------------------------------------------------------

typedef struct {
    char     (*names)[FAT32_NAME_MAX];
    uint32_t  max;
    uint32_t  count;
} ListCtx;

static bool list_entry_fn(const uint8_t *entry, void *user)
{
    ListCtx *ctx = (ListCtx *)user;

    if (ctx->count >= ctx->max) return false;

    // Check extension == "ADF"
    if (entry[8] != 'A' || entry[9] != 'D' || entry[10] != 'F') {
        return true;
    }

    from_83((const char *)entry, ctx->names[ctx->count]);
    ctx->count++;
    return true;
}

uint32_t fat32_list_adf(Fat32State *fs,
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
    char     target[11];
    bool     found;
    uint32_t start_cluster;
    uint32_t file_size;
} OpenCtx;

static bool open_entry_fn(const uint8_t *entry, void *user)
{
    OpenCtx *ctx = (OpenCtx *)user;

    if (memcmp(entry, ctx->target, 11) != 0) return true;

    ctx->start_cluster = ((uint32_t)le16(entry + 20) << 16) | le16(entry + 26);
    ctx->file_size     = le32(entry + 28);
    ctx->found         = true;
    return false;   // stop scanning
}

bool fat32_open(Fat32State *fs, const char *name_dot, Fat32File *out)
{
    if (!fs || !fs->initialized || !name_dot || !out) return false;

    OpenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    to_83(name_dot, ctx.target);

    scan_dir_cluster(fs, fs->root_cluster, open_entry_fn, &ctx);

    if (!ctx.found) {
        kprintf("[FAT32] file not found: %s\n", name_dot);
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
// List ISO files
// ---------------------------------------------------------------------------

static bool list_iso_entry_fn(const uint8_t *entry, void *user)
{
    ListCtx *ctx = (ListCtx *)user;

    if (ctx->count >= ctx->max) return false;

    if (entry[8] != 'I' || entry[9] != 'S' || entry[10] != 'O')
        return true;

    from_83((const char *)entry, ctx->names[ctx->count]);
    ctx->count++;
    return true;
}

uint32_t fat32_list_iso(Fat32State *fs,
                        char        names[][FAT32_NAME_MAX],
                        uint32_t    max_count)
{
    if (!fs || !fs->initialized || !names || max_count == 0u) return 0u;

    ListCtx ctx = { names, max_count, 0u };
    scan_dir_cluster(fs, fs->root_cluster, list_iso_entry_fn, &ctx);
    return ctx.count;
}
