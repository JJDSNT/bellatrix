/*
 * src/machine/machine.c
 *
 * The Bellatrix machine's memory policy.
 *
 * Bellatrix defines the map; Emu68 implements it. Everything here is expressed
 * through the mmu_map() Emu68 already provides -- there are no page tables of
 * our own, and no parallel MMU (docs/Bus.md sections 3 and 10).
 */

#include "machine.h"

#include <stdint.h>

#include "A64.h"
#include "mmu.h"

/*
 * The classic 24-bit Amiga address domain.
 *
 * Bellatrix policy: this range starts inaccessible to direct CPU loads and
 * stores. Individual ranges are then mapped back, one at a time, once they
 * have been confirmed to be normal memory.
 *
 * This inverts what the standalone Emu68 build does. Emu68 maps the advertised
 * system memory 1:1 and punches holes for the few addresses it emulates
 * (src/aarch64/start.c), which leaves everything else -- $E80000 autoconfig,
 * the CIA ranges, every hole in the map -- as ordinary DRAM that reads back
 * whatever happens to be in it. The comment beside the $DFF000 hole says so:
 * "On PiStorm the whole Amiga address space already faults through to the bus;
 * here it is plain RAM."
 *
 * Starting from fault means the machine has to name what it contains, rather
 * than inheriting the host's memory by default. See docs/New_emu68.md
 * section 6 and AI_context/issues/ISSUE-0016.md.
 */
#define MACHINE_LOW24_BASE      0x00000000UL
#define MACHINE_LOW24_SIZE      0x01000000UL

/*
 * Page zero is the one part of that domain this machine does confirm as normal
 * memory, and it has to be: the 68K exception vector table lives at address 0,
 * and AROS reaches its own ExecBase through the absolute long at address 4
 * (the kernel is linked with --defsym AbsExecBase=0x4). Both are read on paths
 * far too hot to service through a fault.
 *
 * arch/m68k-emu68 also leaves a boot-stage marker at 0x400, immediately above
 * the vector table, so a bare-metal monitor can see boot progress before there
 * is a console.
 */
#define MACHINE_PAGE0_BASE      0x00000000UL
#define MACHINE_PAGE0_SIZE      0x00001000UL

/*
 * DIRECT -- normal memory, translated by the MMU, never seen by the fault path.
 * TRAPPED -- no translation the guest may use, so the access reaches machine
 *            semantics.
 *
 * TRAPPED is not "invalid": it is how a machine transaction is delivered
 * (docs/Bus.md section 4).
 *
 * TRAPPED clears MMU_ACCESS and keeps everything else. MMU_ACCESS is 0x400,
 * the AArch64 Access Flag: with it clear the descriptor stays valid and keeps
 * its attributes, and every access to the page raises an Access Flag fault.
 *
 * Two other spellings were tried and both are wrong, so they are recorded here
 * rather than rediscovered:
 *
 *   - Dropping only MMU_ALLOW_EL0 does not trap at all. A deliberate word read
 *     of $E80000 from the AROS bootstrap -- emitted, verified in the object as
 *     `movew e80000,%d0` -- never reached the bus observer. Whatever exception
 *     level the JIT runs guest code at, the EL0 bit is not the gate.
 *
 *   - An attribute-less mapping (0), which is what Emu68's own 4 KiB holes use,
 *     traps everyone. It removed the range from Emu68 as well, and the loader
 *     reading the initrd took 192 faults on its own image before the guest even
 *     started. Right for one page Emu68 never touches; wrong for a range.
 */
#define MACHINE_ATTR_DIRECT     (MMU_ACCESS | MMU_ISHARE | MMU_ALLOW_EL0 | \
                                 MMU_ATTR_CACHED)
#define MACHINE_ATTR_TRAPPED    (MMU_ISHARE | MMU_ALLOW_EL0 | MMU_ATTR_CACHED)

static void machine_setup_memory(void)
{
    /* Deny direct access to the complete classic 24-bit address domain. */
    mmu_map(MACHINE_LOW24_BASE, MACHINE_LOW24_BASE, MACHINE_LOW24_SIZE,
            MACHINE_ATTR_TRAPPED, 0);

    /* Restore only what is known to be normal memory. Order matters: this
     * has to follow the protection above, which covers it. */
    mmu_map(MACHINE_PAGE0_BASE, MACHINE_PAGE0_BASE, MACHINE_PAGE0_SIZE,
            MACHINE_ATTR_DIRECT, 0);

    /*
     * Nothing else is promoted yet, and nothing should be promoted merely
     * because software touched it. An access to a trapped range is a question
     * to answer -- normal RAM, machine MMIO, a mirror, open bus, or a pointer
     * that went wrong -- and only the first answer justifies a mapping.
     */
}

void machine_init(void)
{
    machine_setup_memory();

    kprintf("[BELLATRIX] machine: low 24-bit domain protected, "
            "$%08x-$%08x direct\n",
            (unsigned int)MACHINE_PAGE0_BASE,
            (unsigned int)(MACHINE_PAGE0_BASE + MACHINE_PAGE0_SIZE - 1));
}
