/*
 * The chipset's own core.
 *
 * Emu68 starts all four cores and parks three of them in WFE for the life of
 * the machine (patch emu68/0020 hands this one over). That was affordable
 * while the chipset was cheap and is not any more: measured on a Pi 3, Rigel
 * costs 250 ns per colour clock and realtime wants one every 282, so running
 * it at realtime takes 88.7% of whatever core it shares. On one core a fast
 * CPU and a realtime chipset cannot both exist -- ISSUE-0074, ISSUE-0075.
 *
 * This file is the first increment: prove the core is ours and reachable, and
 * that it can see the machine's state. The chipset does not move here yet.
 * Everything above it -- the lock, the beam snapshot, the posted-write queue --
 * is written against a core that has already been shown to arrive.
 */

#include "amiga/core.h"

#include "A64.h"

#include <stdint.h>

static volatile uint32_t core_arrived;
static volatile uint32_t core_wanted;

void amiga_core_enable(void)
{
    core_wanted = 1;
    __asm__ volatile("dsb ishst\n\tsev" ::: "memory");
}

int amiga_core_arrived(void)
{
    return core_arrived != 0;
}

void bellatrix_chipset_core_entry(void)
{
    uint64_t id;

    __asm__ volatile("mrs %0, MPIDR_EL1" : "=r"(id));
    id &= 3;

    /*
     * Wait to be wanted, rather than checking once.
     *
     * The secondary core reaches here during Emu68's boot -- "Started CPU2" is
     * the thirty-third line of a boot log -- and Bellatrix does not exist yet:
     * amiga_bus_init() runs twenty-five lines later. A core that tests the flag
     * on arrival always finds it clear and parks for good, which is what the
     * first version of this did.
     *
     * Parking here is the same parking Emu68 would do, so a machine that never
     * wants the core is not worse off; it simply never gets the SEV.
     */
    while (!core_wanted)
        __asm__ volatile("wfe" ::: "memory");

    core_arrived = 1;
    __asm__ volatile("dsb ishst\n\tsev" ::: "memory");

    kprintf("[BELLATRIX:RIGEL:CORE] core %u is the chipset's\n", (unsigned)id);

    /*
     * Returning parks the core in Emu68's WFE, which is where it was going
     * anyway. The step that follows is to run the chipset here instead, paced
     * by CNTPCT_EL0 rather than by published CPU cycles -- which is what makes
     * the CPU core free rather than merely less blocked.
     */
}
