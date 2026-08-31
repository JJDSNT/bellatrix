/*
 * src/machine/bus.c
 *
 * Where a faulted guest access enters the machine.
 *
 * One boundary, several destinations. docs/Bus.md section 7 wants the machine
 * entered at a single explicit point; the legacy SPEC-0001 forbids a universal
 * machine callback and wants each branch to reach the owner of its region
 * directly. Both are satisfied by entering here and dispatching immediately on
 * the region table -- what must not exist is a single handler that decides
 * everything, and that is not what this is.
 *
 * Emu68 determines what access happened; this determines what it means
 * (docs/Bus.md section 6).
 */

#include "machine/bus.h"
#include "machine/region.h"

#include <stdint.h>

#include "A64.h"
#include "M68k.h"

/*
 * Report each distinct (pc, address, direction, width) once.
 *
 * A raw log is not usable here, and that is measured rather than feared: a
 * guest sweeping an address range produced 262144 faults in one boot on this
 * project. What identifies an access is the pair (guest PC, address); how
 * often it repeats is a count, not a line.
 */
#define BUS_TABLE_ENTRIES 192

struct bus_entry
{
    uint32_t pc;
    uint32_t address;
    uint32_t count;
    uint8_t  size;
    uint8_t  write;
    uint8_t  used;
};

static struct bus_entry bus_table[BUS_TABLE_ENTRIES];
static uint32_t bus_distinct;
static uint32_t bus_total;
static uint8_t  bus_table_full_reported;
static uint8_t  bus_direct_fault_reported;
static uint8_t  bus_straddle_reported;

/*
 * The guest PC, read the way the rest of this fault path reads it.
 *
 * Emu68 keeps the m68k context pointer in a vector lane -- CTX_POINTER_ASM is
 * "v20.d[1]" -- and the AROS image loads at a known base, so the PC resolves
 * against the ELF this repository builds.
 *
 * What makes this safe is a compile flag, not code here. cmake/bellatrix-
 * variant.cmake gives these sources the same reserved-register set Emu68 gives
 * vectors.c, so the compiler never allocates v20 and the lane still holds what
 * it held. Without that flag no amount of saving and restoring in C would
 * help: the clobber can happen in any function on the path, before this one is
 * even entered.
 */
static uint32_t guest_pc(void)
{
    struct M68KState *m68k;

    __asm__ volatile("mov %0, "CTX_POINTER_ASM"\n" : "=r"(m68k));

    return m68k ? m68k->PC : 0;
}

static struct bus_entry *bus_record(uint32_t address, int size, int write,
                                    uint32_t pc)
{
    unsigned int i;

    for (i = 0; i < BUS_TABLE_ENTRIES; i++)
    {
        struct bus_entry *e = &bus_table[i];

        if (!e->used)
            break;

        if (e->pc == pc && e->address == address &&
            e->write == (uint8_t)!!write && e->size == (uint8_t)size)
        {
            e->count++;
            return 0;
        }
    }

    if (i == BUS_TABLE_ENTRIES)
        return 0;

    bus_table[i].pc = pc;
    bus_table[i].address = address;
    bus_table[i].size = (uint8_t)size;
    bus_table[i].write = (uint8_t)!!write;
    bus_table[i].count = 1;
    bus_table[i].used = 1;
    bus_distinct++;

    return &bus_table[i];
}

static void machine_bus_access(uint32_t address, int size, int write)
{
    const MachineRegion *region;
    MachineAccessFit fit;
    struct bus_entry *fresh;
    uint32_t pc;

    /*
     * Classified by its full width, not by where it starts: the region an
     * access begins in is not necessarily the one it ends in.
     */
    region = machine_region_classify(address, (uint32_t)size, &fit);
    if (!region)
        return;             /* outside the machine's map; Emu68 keeps it */

    bus_total++;
    pc = guest_pc();

    /*
     * An access that leaves the region it began in has no single answer, so it
     * is named rather than silently given the first region's. It is reported
     * once and then handled as the region it starts in, which is what Emu68
     * will do with it regardless -- the value of saying so is that a boundary
     * the machine did not intend to be crossable becomes visible.
     */
    if (fit == MACHINE_ACCESS_STRADDLES && !bus_straddle_reported)
    {
        bus_straddle_reported = 1;
        kprintf("[BELLATRIX] bus: %d-byte access at %08x pc=%08x leaves "
                "'%s' -- classified by its start\n",
                size, address, pc,
                region->name ? region->name : "<unnamed>");
    }

    switch (region->kind)
    {
        case MACHINE_REGION_DIRECT:
            /*
             * A DIRECT region is mapped and must never reach the fault path.
             * Arriving here means the MMU policy and the fault policy have
             * come apart, which docs/Bus.md section 5 makes a mandatory
             * invariant -- so say so once rather than quietly serving it.
             */
            if (!bus_direct_fault_reported)
            {
                bus_direct_fault_reported = 1;
                kprintf("[BELLATRIX] bus: DIRECT region '%s' faulted at "
                        "%08x pc=%08x -- map and fault policy disagree\n",
                        region->name ? region->name : "<unnamed>",
                        address, pc);
            }
            break;

        case MACHINE_REGION_EXTERNAL:
            if (region->ops)
            {
                /* Provider transactions return through machine_bus_read/write.
                 * Reaching the observer means the access straddled a boundary
                 * or the provider did not implement that direction. */
                break;
            }
            /* An EXTERNAL region with no owner is a declaration without an
             * implementation. Fall through and treat it as unclassified. */
            /* fallthrough */

        case MACHINE_REGION_UNMAPPED:
            fresh = bus_record(address, size, write, pc);
            if (fresh)
            {
                kprintf("[BELLATRIX] bus %s%-2d addr=%08x pc=%08x [%s] (#%d)\n",
                        write ? "W" : "R", size * 8, address, pc,
                        region->name ? region->name : "<unnamed>",
                        bus_distinct);
            }
            else if (bus_distinct == BUS_TABLE_ENTRIES &&
                     !bus_table_full_reported)
            {
                bus_table_full_reported = 1;
                kprintf("[BELLATRIX] bus: table full at %d distinct accesses "
                        "-- further ones count toward the total only\n",
                        bus_distinct);
            }
            break;
    }
}

static uint64_t machine_open_bus_value(int size)
{
    switch (size)
    {
        case 1:  return 0xffu;
        case 2:  return 0xffffu;
        case 4:  return 0xffffffffu;
        default: return UINT64_MAX;
    }
}

int machine_bus_read(uint32_t address, int size, uint64_t *value)
{
    const MachineRegion *region;
    MachineAccessFit fit;

    region = machine_region_classify(address, (uint32_t)size, &fit);
    if (region && fit == MACHINE_ACCESS_INSIDE &&
        region->kind == MACHINE_REGION_EXTERNAL && region->ops &&
        region->ops->read)
    {
        *value = region->ops->read(region, address, size);
        return 1;
    }

    machine_bus_access(address, size, 0);

    /*
     * UNMAPPED is a machine decision, not an invitation to let Emu68 touch
     * the still-faulting address.  Complete the transaction as open bus after
     * recording it.  This also keeps the boot-time classic-domain probe useful
     * without recursively entering the data-abort handler.
     */
    if (region && fit == MACHINE_ACCESS_INSIDE &&
        (region->kind == MACHINE_REGION_UNMAPPED ||
         (region->kind == MACHINE_REGION_EXTERNAL && !region->ops)))
    {
        *value = machine_open_bus_value(size);
        return 1;
    }

    return 0;
}

int machine_bus_write(uint32_t address, int size, uint64_t value)
{
    const MachineRegion *region;
    MachineAccessFit fit;

    region = machine_region_classify(address, (uint32_t)size, &fit);
    if (region && fit == MACHINE_ACCESS_INSIDE &&
        region->kind == MACHINE_REGION_EXTERNAL && region->ops &&
        region->ops->write)
    {
        region->ops->write(region, address, size, (uint32_t)value);
        return 1;
    }

    machine_bus_access(address, size, 1);

    if (region && fit == MACHINE_ACCESS_INSIDE &&
        (region->kind == MACHINE_REGION_UNMAPPED ||
         (region->kind == MACHINE_REGION_EXTERNAL && !region->ops)))
        return 1;

    return 0;
}

void machine_bus_report(void)
{
    unsigned int i;

    kprintf("[BELLATRIX] bus summary: %d accesses, %d distinct\n",
            bus_total, bus_distinct);

    for (i = 0; i < BUS_TABLE_ENTRIES && bus_table[i].used; i++)
    {
        const struct bus_entry *e = &bus_table[i];

        kprintf("[BELLATRIX]   %s%-2d addr=%08x pc=%08x count=%d\n",
                e->write ? "W" : "R", e->size * 8, e->address, e->pc,
                e->count);
    }
}
