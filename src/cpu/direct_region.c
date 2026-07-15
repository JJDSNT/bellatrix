#include "cpu/direct_region.h"

#include <string.h>

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
