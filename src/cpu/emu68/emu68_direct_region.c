#include "cpu/emu68/emu68_direct_region.h"

#include "A64.h"
#include "mmu.h"

static BellatrixDirectRegionMap s_direct_regions;

static int emu68_mmu_map(void *opaque, const BellatrixDirectRegion *region)
{
    uintptr_t phys;
    uint32_t attr = MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0;

    (void)opaque;
    phys = mmu_virt2phys((uintptr_t)region->host_base);
    if (phys == (uintptr_t)-1)
        return -1;
    attr |= (region->flags & BELLATRIX_DIRECT_CACHEABLE) ? MMU_ATTR_CACHED :
                                                           MMU_ATTR_UNCACHED;
    if ((region->flags & BELLATRIX_DIRECT_WRITE) == 0u)
        attr |= MMU_READ_ONLY;
    mmu_map(phys, region->guest_base, region->size, attr, 0u);
    return 0;
}

static int emu68_mmu_unmap(void *opaque, const BellatrixDirectRegion *region)
{
    uintptr_t phys;
    uint32_t attr = MMU_ACCESS | MMU_ISHARE;

    (void)opaque;
    phys = mmu_virt2phys((uintptr_t)region->host_base);
    if (phys == (uintptr_t)-1)
        return -1;

    /* Native Emu68 has no usable page-removal primitive. Restore the native
     * Data-Abort contract by replacing the EL0 mapping with an EL1-only one.
     * The physical backing remains reachable by ARM, while every 68k access
     * faults through vectors.c again. This deliberately does not add a lookup
     * to either the vectors or fault hot path. */
    attr |= (region->flags & BELLATRIX_DIRECT_CACHEABLE) ? MMU_ATTR_CACHED :
                                                           MMU_ATTR_UNCACHED;
    mmu_map(phys, region->guest_base, region->size, attr, 0u);
    return 0;
}

void bellatrix_emu68_direct_region_init(void)
{
    static const BellatrixDirectRegionBackendOps ops = {
        .map = emu68_mmu_map,
        .unmap = emu68_mmu_unmap,
    };
    bellatrix_direct_region_map_init(&s_direct_regions, &ops, NULL);
}

int bellatrix_emu68_direct_region_install(const BellatrixDirectRegion *region)
{
    return bellatrix_direct_region_install(&s_direct_regions, region);
}

int bellatrix_emu68_direct_region_remove(uint32_t guest_base, uint32_t size)
{
    return bellatrix_direct_region_remove(&s_direct_regions, guest_base, size);
}
