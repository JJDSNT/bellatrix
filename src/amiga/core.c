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
#include "amiga/bus.h"

#include "A64.h"

#include <stdatomic.h>
#include <stdint.h>

static volatile uint32_t core_arrived;
static volatile uint32_t core_wanted;

static atomic_flag chipset_lock = ATOMIC_FLAG_INIT;
static _Atomic uint32_t chipset_lock_waiters;

int amiga_core_owns_chipset(void)
{
    return core_arrived != 0;
}

void amiga_core_lock_acquire(void)
{
    if (!atomic_flag_test_and_set_explicit(&chipset_lock, memory_order_acquire))
        return;

    atomic_fetch_add_explicit(&chipset_lock_waiters, 1u, memory_order_relaxed);
    while (atomic_flag_test_and_set_explicit(&chipset_lock, memory_order_acquire))
        __asm__ volatile("wfe" ::: "memory");
    atomic_fetch_sub_explicit(&chipset_lock_waiters, 1u, memory_order_relaxed);
}

void amiga_core_lock_release(void)
{
    atomic_flag_clear_explicit(&chipset_lock, memory_order_release);

    /*
     * Only signal when a waiter actually parked.
     *
     * Almost every release is the chipset core finishing an uncontended step,
     * and the legacy implementation's unconditional broadcast woke every PE
     * and turned work completions into empty loop iterations on cores that had
     * nothing to do. The release store is the lock's ordering contract; the
     * event is only needed by someone in WFE.
     */
    if (atomic_load_explicit(&chipset_lock_waiters, memory_order_relaxed) != 0u)
        __asm__ volatile("dmb ishst\n\tsev" ::: "memory");
}

void amiga_core_enable(void)
{
#if defined(CONFIG_RIGEL_CHIPSET_CORE) && CONFIG_RIGEL_CHIPSET_CORE == 0
    /*
     * Built without the chipset core, deliberately.
     *
     * Two things changed at once when the chipset moved: its clock became a
     * function of real time, and it moved to a core of its own. A boot that
     * stops cannot say which, and this switch is what splits them: the same
     * wall-clock chipset, on the CPU core, so a failure that survives is not
     * about concurrency and one that disappears is.
     */
    kprintf("[BELLATRIX:RIGEL:CORE] chipset core disabled by build\n");
    return;
#else
    core_wanted = 1;
    __asm__ volatile("dsb ishst\n\tsev" ::: "memory");
#endif
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

    /* Does not return: this core is the chipset from here on. */
    amiga_clock_run_on_core();
}
