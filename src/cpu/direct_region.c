#include "cpu/direct_region.h"

#include <string.h>

/*
 * Sparse DIRECT-region control plane shared by CPU backends.
 *
 * Emu68 uses install/remove only: the backend turns them into MMU mappings,
 * then normal 68k loads/stores bypass this table entirely. Do not call find()
 * from vectors.c or the Emu68 Data Abort path.
 *
 * Musashi has no equivalent host-MMU datapath, so its memory callbacks may use
 * find() to locate the already-installed bank/buffer. The lookup cost is thus
 * specific to Musashi, not imposed on Emu68.
 */

#define DIRECT_KNOWN_FLAGS (BELLATRIX_DIRECT_READ | BELLATRIX_DIRECT_WRITE | \
                            BELLATRIX_DIRECT_EXECUTE |                       \
                            BELLATRIX_DIRECT_CACHEABLE)

static int region_valid(const BellatrixDirectRegion *region)
{
    uint64_t end;

    if (!region || !region->host_base || region->size == 0u)
        return 0;
    if ((region->guest_base & (BELLATRIX_DIRECT_PAGE_SIZE - 1u)) != 0u ||
        (region->size & (BELLATRIX_DIRECT_PAGE_SIZE - 1u)) != 0u ||
        ((uintptr_t)region->host_base & (BELLATRIX_DIRECT_PAGE_SIZE - 1u)) != 0u)
        return 0;
    if ((region->flags & BELLATRIX_DIRECT_READ) == 0u ||
        (region->flags & ~DIRECT_KNOWN_FLAGS) != 0u)
        return 0;
    end = (uint64_t)region->guest_base + region->size;
    return end <= UINT64_C(0x100000000);
}

static int regions_overlap(const BellatrixDirectRegion *a,
                           const BellatrixDirectRegion *b)
{
    uint64_t a_end = (uint64_t)a->guest_base + a->size;
    uint64_t b_end = (uint64_t)b->guest_base + b->size;
    return (uint64_t)a->guest_base < b_end &&
           (uint64_t)b->guest_base < a_end;
}

void bellatrix_direct_region_map_init(
    BellatrixDirectRegionMap *map,
    const BellatrixDirectRegionBackendOps *ops,
    void *opaque)
{
    if (!map)
        return;
    memset(map, 0, sizeof(*map));
    map->ops = ops;
    map->opaque = opaque;
}

int bellatrix_direct_region_install(
    BellatrixDirectRegionMap *map,
    const BellatrixDirectRegion *region)
{
    size_t free_slot = BELLATRIX_DIRECT_REGION_CAPACITY;
    size_t i;

    if (!map || !map->ops || !map->ops->map || !region_valid(region))
        return -1;
    for (i = 0; i < BELLATRIX_DIRECT_REGION_CAPACITY; ++i) {
        if (!map->slots[i].used) {
            if (free_slot == BELLATRIX_DIRECT_REGION_CAPACITY)
                free_slot = i;
            continue;
        }
        if (regions_overlap(&map->slots[i].region, region))
            return -2;
    }
    if (free_slot == BELLATRIX_DIRECT_REGION_CAPACITY)
        return -3;
    if (map->ops->map(map->opaque, region) != 0)
        return -4;
    map->slots[free_slot].region = *region;
    map->slots[free_slot].used = 1u;
    return 0;
}

int bellatrix_direct_region_remove(
    BellatrixDirectRegionMap *map,
    uint32_t guest_base,
    uint32_t size)
{
    size_t i;

    if (!map || !map->ops || !map->ops->unmap)
        return -1;
    for (i = 0; i < BELLATRIX_DIRECT_REGION_CAPACITY; ++i) {
        BellatrixDirectRegionSlot *slot = &map->slots[i];
        if (!slot->used || slot->region.guest_base != guest_base ||
            slot->region.size != size)
            continue;
        if (map->ops->unmap(map->opaque, &slot->region) != 0)
            return -2;
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    return -3;
}

const BellatrixDirectRegion *bellatrix_direct_region_find(
    const BellatrixDirectRegionMap *map,
    uint32_t address,
    uint32_t access_size)
{
    uint64_t access_end;
    size_t i;

    if (!map || access_size == 0u)
        return NULL;
    access_end = (uint64_t)address + access_size;
    if (access_end > UINT64_C(0x100000000))
        return NULL;
    for (i = 0; i < BELLATRIX_DIRECT_REGION_CAPACITY; ++i) {
        const BellatrixDirectRegionSlot *slot = &map->slots[i];
        uint64_t region_end;
        if (!slot->used)
            continue;
        region_end = (uint64_t)slot->region.guest_base + slot->region.size;
        if (address >= slot->region.guest_base && access_end <= region_end)
            return &slot->region;
    }
    return NULL;
}

int bellatrix_direct_region_read(
    const BellatrixDirectRegionMap *map,
    uint32_t address,
    unsigned int size,
    uint32_t *value_out)
{
    const BellatrixDirectRegion *region;
    const uint8_t *data;
    uint32_t offset;

    if (!value_out || (size != 1u && size != 2u && size != 4u))
        return 0;
    region = bellatrix_direct_region_find(map, address, size);
    if (!region || (region->flags & BELLATRIX_DIRECT_READ) == 0u)
        return 0;
    offset = address - region->guest_base;
    data = (const uint8_t *)region->host_base + offset;
    if (size == 1u)
        *value_out = data[0];
    else if (size == 2u)
        *value_out = ((uint32_t)data[0] << 8) | data[1];
    else
        *value_out = ((uint32_t)data[0] << 24) |
                     ((uint32_t)data[1] << 16) |
                     ((uint32_t)data[2] << 8) | data[3];
    return 1;
}

int bellatrix_direct_region_write(
    const BellatrixDirectRegionMap *map,
    uint32_t address,
    unsigned int size,
    uint32_t value)
{
    const BellatrixDirectRegion *region;
    uint8_t *data;
    uint32_t offset;

    if (size != 1u && size != 2u && size != 4u)
        return 0;
    region = bellatrix_direct_region_find(map, address, size);
    if (!region)
        return 0;
    if ((region->flags & BELLATRIX_DIRECT_WRITE) == 0u)
        return 1;
    offset = address - region->guest_base;
    data = (uint8_t *)region->host_base + offset;
    if (size == 1u) {
        data[0] = (uint8_t)value;
    } else if (size == 2u) {
        data[0] = (uint8_t)(value >> 8);
        data[1] = (uint8_t)value;
    } else {
        data[0] = (uint8_t)(value >> 24);
        data[1] = (uint8_t)(value >> 16);
        data[2] = (uint8_t)(value >> 8);
        data[3] = (uint8_t)value;
    }
    return 1;
}
