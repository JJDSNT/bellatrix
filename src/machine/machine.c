/*
 * src/machine/machine.c
 *
 * What the Bellatrix machine's address space contains.
 *
 * This file is the policy and nothing else. It names regions; region.c turns
 * them into MMU state through the mmu_map() Emu68 already provides, and the
 * fault path asks the same table what an address means. Bellatrix defines the
 * map; Emu68 implements it (docs/Bus.md sections 3 and 10).
 */

#include "machine/machine.h"
#include "machine/region.h"

#include <stdint.h>

#include "A64.h"

/*
 * The classic 24-bit Amiga address domain, $000000-$FFFFFF.
 *
 * It starts inaccessible. Ranges are promoted to a direct mapping one at a
 * time, and only once this machine has decided what they are -- never because
 * software touched one (docs/New_emu68.md sections 6 and 9).
 *
 * That inverts what a standalone Emu68 does. Emu68 maps the advertised system
 * memory 1:1 and punches holes for the few addresses it emulates, which leaves
 * $E80000 autoconfig, the CIA ranges and every hole in the map as ordinary
 * DRAM reading back whatever happens to be in it. Emu68's own comment beside
 * the $DFF000 hole is explicit: "On PiStorm the whole Amiga address space
 * already faults through to the bus; here it is plain RAM."
 *
 * Starting from fault means the machine has to say what it contains rather
 * than inheriting its host's memory by default.
 */
#define CLASSIC_DOMAIN_BASE     0x00000000UL
#define CLASSIC_DOMAIN_SIZE     0x01000000UL

/*
 * Page zero is the one part of that domain confirmed as normal memory, and it
 * has to be: the 68K exception vector table is at address 0 and AROS reaches
 * its own ExecBase through the absolute long at address 4 (the kernel is
 * linked with --defsym AbsExecBase=0x4). Both are read on paths far too hot to
 * service through a fault. arch/m68k-emu68 also leaves a boot-stage marker at
 * 0x400, immediately above the vector table.
 *
 * Its backing is host physical 0. That is written as a separate field from the
 * guest base and stays that way even though the two are equal here: the moment
 * a region is backed somewhere other than its own address -- a mirror, a
 * prepared ROM window, a board -- the distinction is the whole design, and the
 * legacy notes are emphatic that conflating them cost real time.
 */
#define VECTOR_PAGE_BASE        0x00000000UL
#define VECTOR_PAGE_SIZE        0x00001000UL
#define VECTOR_PAGE_HOST_PHYS   0x00000000UL

static const MachineRegion machine_map[] =
{
    {
        .base      = VECTOR_PAGE_BASE,
        .size      = VECTOR_PAGE_SIZE,
        .kind      = MACHINE_REGION_DIRECT,
        .name      = "vectors + AbsExecBase",
        .host_phys = VECTOR_PAGE_HOST_PHYS,
        .attr      = MMU_ATTR_CACHED,
    },
    {
        /*
         * Everything else in the classic domain. Not a statement that this is
         * one thing -- it is the absence of statements about it. As accesses
         * are attributed and classified, ranges come out of here and become
         * regions of their own.
         */
        .base = VECTOR_PAGE_BASE + VECTOR_PAGE_SIZE,
        .size = CLASSIC_DOMAIN_SIZE - VECTOR_PAGE_SIZE,
        .kind = MACHINE_REGION_UNMAPPED,
        .name = "classic domain, unclassified",
    },
};

#define MACHINE_MAP_ENTRIES (sizeof(machine_map) / sizeof(machine_map[0]))

static void machine_setup_memory(void)
{
    unsigned int i;

    for (i = 0; i < MACHINE_MAP_ENTRIES; i++)
        machine_region_install(&machine_map[i]);
}

void machine_init(void)
{
    machine_setup_memory();
    machine_region_report();
}
