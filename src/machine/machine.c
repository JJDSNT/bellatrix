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
#include "machine/memory.h"
#include "machine/options.h"
#include "machine/region.h"
#include "machine/vecpage.h"
#if CONFIG_RIGEL
#include "amiga/bus.h"
#include "amiga/frame.h"
#endif

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

#if CONFIG_RIGEL
/*
 * With the chipset in the machine that page is also the first page of chip
 * RAM, and the guest's allocation floor sits immediately above it. Say so,
 * rather than repeating 0x1000 in a second constant that happens to agree:
 * two definitions of one page is how a map and an allocator come to disagree
 * by exactly one page and nothing says which is wrong.
 */
_Static_assert(AMIGA_CHIP_RAM_BASE == VECTOR_PAGE_BASE,
               "the vector page is the first page of chip RAM");
_Static_assert(AMIGA_CHIP_RAM_ALLOC_BASE == VECTOR_PAGE_BASE + VECTOR_PAGE_SIZE,
               "the guest allocates chip RAM from above the vector page");

#define CIA_BASE                0x00BFD000UL
#define CIA_SIZE                0x00002000UL
#define RTC_BASE                0x00DC0000UL
#define RTC_SIZE                0x00010000UL
#define CUSTOM_BASE             0x00DFF000UL
#define CUSTOM_SIZE             0x00001000UL
#endif

/*
 * The machine without a chipset.
 *
 * The vector page is not described here: it is installed by
 * machine_setup_memory() below, because whether it is direct or fault-driven
 * is a boot-time decision and the table refuses overlapping descriptions.
 * Everything above it is the absence of statements rather than a statement
 * that it is one thing -- as accesses are attributed and classified, ranges
 * come out of here and become regions of their own.
 */
static const MachineRegion plain_map[] =
{
    {
        .base = VECTOR_PAGE_BASE + VECTOR_PAGE_SIZE,
        .size = CLASSIC_DOMAIN_SIZE - VECTOR_PAGE_SIZE,
        .kind = MACHINE_REGION_UNMAPPED,
        .name = "classic domain, unclassified",
    },
};

#if CONFIG_RIGEL
/*
 * The machine with one: the low 24 bits belong to the classic chipset.
 *
 * Chip RAM is direct, minus its first page for the same reason as above; the
 * three apertures are external and decode through Rigel; everything between
 * them stays unmapped, so an access to a range nothing has claimed still
 * reaches machine semantics instead of reading DRAM back.
 */
static const MachineRegion classic_map[] =
{
    {
        .base      = VECTOR_PAGE_BASE + VECTOR_PAGE_SIZE,
        .size      = AMIGA_CHIP_RAM_SIZE - VECTOR_PAGE_SIZE,
        .kind      = MACHINE_REGION_DIRECT,
        .name      = "Chip RAM",
        .host_phys = VECTOR_PAGE_HOST_PHYS + VECTOR_PAGE_SIZE,
        .attr      = MMU_ATTR_CACHED,
    },
    {
        .base = AMIGA_CHIP_RAM_BASE + AMIGA_CHIP_RAM_SIZE,
        .size = CIA_BASE - (AMIGA_CHIP_RAM_BASE + AMIGA_CHIP_RAM_SIZE),
        .kind = MACHINE_REGION_UNMAPPED,
        .name = "classic domain before CIA",
    },
    {
        .base = CIA_BASE,
        .size = CIA_SIZE,
        .kind = MACHINE_REGION_EXTERNAL,
        .name = "Rigel CIA aperture",
        .ops = &amiga_bus_ops,
    },
    {
        .base = CIA_BASE + CIA_SIZE,
        .size = RTC_BASE - (CIA_BASE + CIA_SIZE),
        .kind = MACHINE_REGION_UNMAPPED,
        .name = "classic domain between CIA and RTC",
    },
    {
        .base = RTC_BASE,
        .size = RTC_SIZE,
        .kind = MACHINE_REGION_EXTERNAL,
        .name = "Rigel RTC aperture",
        .ops = &amiga_bus_ops,
    },
    {
        .base = RTC_BASE + RTC_SIZE,
        .size = CUSTOM_BASE - (RTC_BASE + RTC_SIZE),
        .kind = MACHINE_REGION_UNMAPPED,
        .name = "classic domain between RTC and custom chips",
    },
    {
        .base = CUSTOM_BASE,
        .size = CUSTOM_SIZE,
        .kind = MACHINE_REGION_EXTERNAL,
        .name = "Rigel custom-chip aperture",
        .ops = &amiga_bus_ops,
    },
    {
        .base = CUSTOM_BASE + CUSTOM_SIZE,
        .size = (CLASSIC_DOMAIN_BASE + CLASSIC_DOMAIN_SIZE) -
                (CUSTOM_BASE + CUSTOM_SIZE),
        .kind = MACHINE_REGION_UNMAPPED,
        .name = "classic domain after custom chips",
    },
};
#endif

#define ARRAY_ENTRIES(a) (sizeof(a) / sizeof((a)[0]))

static void machine_setup_memory(void)
{
    const MachineRegion *map = plain_map;
    unsigned int entries = ARRAY_ENTRIES(plain_map);
    unsigned int i;

#if CONFIG_RIGEL
    if (bellatrix_rigel_enabled())
    {
        map = classic_map;
        entries = ARRAY_ENTRIES(classic_map);
    }
#endif

    for (i = 0; i < entries; i++)
        machine_region_install(&map[i]);

    /*
     * The vector page, whose kind is a boot-time decision rather than a
     * compile-time one -- and which is the one page both compositions agree
     * on, so it is described here once instead of in each map.
     *
     * DIRECT is the normal answer and the legacy tree explains why it has to
     * be: a write-trap here produced store-buffer coherency failures between
     * the host's alias and the guest's own mapping. Fault-driven is the
     * diagnostic for ISSUE-0082, where the question is who writes AbsExecBase
     * -- and that question has no cheaper instrument, because a DIRECT page is
     * by definition one the fault path never sees.
     */
    {
        MachineRegion vectors =
        {
            .base      = VECTOR_PAGE_BASE,
            .size      = VECTOR_PAGE_SIZE,
            .host_phys = VECTOR_PAGE_HOST_PHYS,
            .attr      = MMU_ATTR_CACHED,
        };

        if (machine_vecpage_trapped())
        {
            vectors.kind = MACHINE_REGION_EXTERNAL;
            vectors.name = "vectors + AbsExecBase (fault-driven)";
            vectors.ops  = &machine_vecpage_ops;
        }
        else
        {
            vectors.kind = MACHINE_REGION_DIRECT;
            vectors.name = "vectors + AbsExecBase";
        }

        machine_region_install(&vectors);
    }
}

void machine_init(void)
{
    /*
     * What was asked for, before anything is built from it. A log that says
     * which machine this boot is comes ahead of the map it produced.
     */
    bellatrix_options_report();

    machine_setup_memory();
#if CONFIG_RIGEL
    if (bellatrix_rigel_enabled())
    {
        /*
         * Order matters, and it is not the obvious one.
         *
         * The frame aperture is installed after the static map, because it
         * adds regions of its own and the table refuses an overlap -- which is
         * the point of the table. But it is installed *before* the chipset,
         * because amiga_bus_init() may run a selftest that programs a display,
         * and a frame composed before there is an aperture to publish it into
         * is a frame the guest never sees. That is not hypothetical: it is
         * what the first run of the DeniseView probe reported.
         */
        amiga_frame_init();
        amiga_bus_init();
    }
#endif
    machine_region_report();
}
