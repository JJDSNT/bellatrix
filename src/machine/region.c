/*
 * src/machine/region.c
 *
 * The region table, and the single point where it becomes MMU state.
 *
 * Bellatrix defines the map; Emu68 implements it. Nothing here maintains page
 * tables or duplicates translation -- it drives the mmu_map() Emu68 already
 * provides (docs/Bus.md sections 3 and 10).
 */

#include "machine/region.h"

#include "A64.h"
#include "mmu.h"

#ifndef MACHINE_REGION_CAPACITY
#define MACHINE_REGION_CAPACITY 32
#endif

#define MACHINE_PAGE_SIZE 0x1000u

static MachineRegion regions[MACHINE_REGION_CAPACITY];
static unsigned int region_count;

static const char *kind_name(MachineRegionKind kind)
{
    switch (kind)
    {
        case MACHINE_REGION_DIRECT:   return "DIRECT";
        case MACHINE_REGION_EXTERNAL: return "EXTERNAL";
        case MACHINE_REGION_UNMAPPED: return "UNMAPPED";
    }
    return "?";
}

/*
 * The kind decides the access flag; the region decides everything else.
 *
 * MMU_ACCESS is 0x400, the AArch64 Access Flag. Clearing it leaves the
 * descriptor valid and carrying its attributes while every access to the page
 * raises an Access Flag fault, which is exactly what a trapped range needs.
 *
 * Two other spellings were measured and both are wrong, recorded here so they
 * are not tried again. Dropping MMU_ALLOW_EL0 instead traps nothing: a
 * deliberate word read of $E80000 from the AROS bootstrap, verified in the
 * object as `movew e80000,%d0`, went straight through. An attribute-less
 * mapping traps everyone including Emu68, whose ELF loader then took 192
 * faults reading the initrd out of the low addresses before the guest existed.
 */
static uint32_t attrs_for(const MachineRegion *region)
{
    uint32_t attr = MMU_ISHARE | MMU_ALLOW_EL0 | region->attr;

    if (region->kind == MACHINE_REGION_DIRECT)
        attr |= MMU_ACCESS;

    return attr;
}

static int overlaps(const MachineRegion *a, const MachineRegion *b)
{
    return a->base < b->base + b->size && b->base < a->base + a->size;
}

int machine_region_install(const MachineRegion *region)
{
    unsigned int i;
    uintptr_t phys;

    if (!region || region->size == 0 ||
        (region->base & (MACHINE_PAGE_SIZE - 1)) ||
        (region->size & (MACHINE_PAGE_SIZE - 1)))
    {
        kprintf("[BELLATRIX] region '%s' refused: %08x+%08x is not whole pages\n",
                region && region->name ? region->name : "<unnamed>",
                region ? region->base : 0, region ? region->size : 0);
        return -1;
    }

    for (i = 0; i < region_count; i++)
    {
        if (overlaps(region, &regions[i]))
        {
            /*
             * Two descriptions of the same address is the ambiguity this table
             * exists to prevent, so it is refused rather than resolved by
             * order of arrival. Whoever wrote the second one believed
             * something about the first that is not true.
             */
            kprintf("[BELLATRIX] region '%s' refused: %08x+%08x overlaps '%s'\n",
                    region->name ? region->name : "<unnamed>",
                    region->base, region->size,
                    regions[i].name ? regions[i].name : "<unnamed>");
            return -1;
        }
    }

    if (region_count == MACHINE_REGION_CAPACITY)
    {
        kprintf("[BELLATRIX] region '%s' refused: table full (%d)\n",
                region->name ? region->name : "<unnamed>",
                MACHINE_REGION_CAPACITY);
        return -1;
    }

    regions[region_count++] = *region;

    /*
     * A trapped region still needs a descriptor -- it is the access flag that
     * makes it fault, not the absence of a mapping -- so map it identity. Its
     * host_phys means nothing, and is deliberately not consulted.
     */
    phys = (region->kind == MACHINE_REGION_DIRECT) ? region->host_phys
                                                   : region->base;

    mmu_map(phys, region->base, region->size, attrs_for(region), 0);

    return 0;
}

const MachineRegion *machine_region_find(uint32_t address)
{
    unsigned int i;

    for (i = 0; i < region_count; i++)
    {
        if (address >= regions[i].base &&
            address - regions[i].base < regions[i].size)
            return &regions[i];
    }

    return 0;
}

const MachineRegion *machine_region_classify(uint32_t address, uint32_t size,
                                             MachineAccessFit *fit)
{
    const MachineRegion *region = machine_region_find(address);
    uint32_t offset;

    if (!region)
    {
        if (fit)
            *fit = MACHINE_ACCESS_NONE;
        return 0;
    }

    /*
     * Compared in offsets rather than by adding size to the address, so that
     * an access at the very top of the space cannot wrap round to zero and
     * report itself as fitting.
     */
    offset = address - region->base;

    if (size == 0 || size > region->size - offset)
    {
        if (fit)
            *fit = MACHINE_ACCESS_STRADDLES;
        return region;
    }

    if (fit)
        *fit = MACHINE_ACCESS_INSIDE;

    return region;
}

void machine_region_report(void)
{
    unsigned int i;

    kprintf("[BELLATRIX] machine map, %d regions:\n", region_count);

    for (i = 0; i < region_count; i++)
    {
        const MachineRegion *r = &regions[i];

        kprintf("[BELLATRIX]   $%08x-$%08x %-8s %s",
                r->base, r->base + r->size - 1, kind_name(r->kind),
                r->name ? r->name : "<unnamed>");

        if (r->kind == MACHINE_REGION_DIRECT)
            kprintf(" (host $%08x)", (uint32_t)r->host_phys);

        kprintf("\n");
    }
}
