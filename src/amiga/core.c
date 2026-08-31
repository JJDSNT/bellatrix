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

int amiga_core_owns_chipset(void)
{
    return core_arrived != 0;
}

/*
 * Spin, do not sleep.
 *
 * This was WFE with a waiter count, and a release that only sent the event
 * when the count was non-zero -- which is what the legacy tree did, and saves
 * waking every core for an uncontended release. It also has a lost wakeup in
 * it: the release's relaxed load of the count may be ordered before its own
 * clear, so a holder can read zero, skip the event, and leave a waiter parked
 * in WFE on a lock that is already free.
 *
 * That is the third time tonight a WFE without a guaranteed event has stopped
 * this machine, and the first two were mine as well. The console drainer waited
 * for an event nobody sent; the chipset core waited for a flag with no event
 * behind it. All three were invisible under QEMU, where WFE returns
 * immediately and the whole class of bug does not exist.
 *
 * Spinning is affordable here in a way it was not for the original: the
 * critical section is bounded at about a millisecond of chipset work by the
 * budget in amiga_clock_run_on_core(). A waiter burns a core for at most that,
 * and it cannot be lost.
 */
void amiga_core_lock_acquire(void)
{
    while (atomic_flag_test_and_set_explicit(&chipset_lock, memory_order_acquire))
        __asm__ volatile("yield" ::: "memory");
}

void amiga_core_lock_release(void)
{
    atomic_flag_clear_explicit(&chipset_lock, memory_order_release);
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
