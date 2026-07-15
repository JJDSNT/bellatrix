#ifndef BELLATRIX_CPU_DIRECT_REGION_H
#define BELLATRIX_CPU_DIRECT_REGION_H

#include <stddef.h>
#include <stdint.h>

#ifndef BELLATRIX_DIRECT_REGION_CAPACITY
#define BELLATRIX_DIRECT_REGION_CAPACITY 16u
#endif

#define BELLATRIX_DIRECT_PAGE_SIZE 0x1000u

enum {
    BELLATRIX_DIRECT_READ      = 1u << 0,
    BELLATRIX_DIRECT_WRITE     = 1u << 1,
    BELLATRIX_DIRECT_EXECUTE   = 1u << 2,
    BELLATRIX_DIRECT_CACHEABLE = 1u << 3
};

typedef struct BellatrixDirectRegion {
    uint32_t guest_base;
    uint32_t size;
    void *host_base;
    uint32_t flags;
} BellatrixDirectRegion;

typedef struct BellatrixDirectRegionBackendOps {
    int (*map)(void *opaque, const BellatrixDirectRegion *region);
    int (*unmap)(void *opaque, const BellatrixDirectRegion *region);
} BellatrixDirectRegionBackendOps;

typedef struct BellatrixDirectRegionSlot {
    BellatrixDirectRegion region;
    uint8_t used;
} BellatrixDirectRegionSlot;

typedef struct BellatrixDirectRegionMap {
    const BellatrixDirectRegionBackendOps *ops;
    void *opaque;
    BellatrixDirectRegionSlot slots[BELLATRIX_DIRECT_REGION_CAPACITY];
} BellatrixDirectRegionMap;

/* Shared control-plane registry. Emu68 uses install/remove to manage its MMU
 * and MUST NOT call find() for steady-state loads/stores or Data Abort. Musashi
 * may use find() from its memory callbacks because it has no host-MMU datapath. */

void bellatrix_direct_region_map_init(
    BellatrixDirectRegionMap *map,
    const BellatrixDirectRegionBackendOps *ops,
    void *opaque);

int bellatrix_direct_region_install(
    BellatrixDirectRegionMap *map,
    const BellatrixDirectRegion *region);

int bellatrix_direct_region_remove(
    BellatrixDirectRegionMap *map,
    uint32_t guest_base,
    uint32_t size);

const BellatrixDirectRegion *bellatrix_direct_region_find(
    const BellatrixDirectRegionMap *map,
    uint32_t address,
    uint32_t access_size);

/* Musashi-style callback helpers. Return 1 when a DIRECT region owns the
 * access. Writes to read-only regions are handled and intentionally ignored. */
int bellatrix_direct_region_read(
    const BellatrixDirectRegionMap *map,
    uint32_t address,
    unsigned int size,
    uint32_t *value_out);

int bellatrix_direct_region_write(
    const BellatrixDirectRegionMap *map,
    uint32_t address,
    unsigned int size,
    uint32_t value);

#endif
