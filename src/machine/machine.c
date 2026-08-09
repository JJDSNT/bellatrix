/* src/machine/machine.c */

#include "machine.h"

#include <stdint.h>
#include <stddef.h>

#include "mmu.h"

/*
 * Classic 24-bit Amiga address domain.
 *
 * Bellatrix policy:
 *
 *   0x00000000 - 0x00ffffff
 *
 * starts inaccessible to direct CPU memory accesses.
 *
 * Individual ranges are then explicitly mapped back when they are
 * confirmed to be normal memory.
 *
 * Everything else remains faulting and is therefore observable through
 * the Emu68 data-abort path / Amiga bus hook.
 */

#define AMIGA24_BASE       0x00000000UL
#define AMIGA24_SIZE       0x01000000UL

/*
 * Initial known direct-memory region.
 *
 * Example: 2 MiB Chip RAM.
 */
#define CHIPRAM_BASE       0x00000000UL
#define CHIPRAM_SIZE       0x00200000UL


/*
 * These attributes are placeholders for the actual normal-RAM
 * attributes already used by the pinned Emu68 tree.
 *
 * Do not introduce Bellatrix-specific page-table attributes here.
 * Reuse Emu68's existing definitions/constants.
 */
#define MMU_NORMAL_LO      /* existing Emu68 normal-memory attr */
#define MMU_NORMAL_HI      /* existing Emu68 normal-memory attr */


/*
 * Host/physical backing of Chip RAM.
 *
 * machine_init() should receive this from the existing Bellatrix
 * memory/bootstrap code instead if that fits the current ownership
 * model better.
 */
static uintptr_t s_chipram_phys;


/*
 * Step 1:
 *
 * Make the complete 24-bit domain faulting.
 *
 * In the current Emu68 approach this is done with the same mmu_map()
 * mechanism already used to create no-access holes: zero attributes.
 *
 * This does NOT mean "16 MiB of MMIO".
 *
 * It means:
 *
 *      no low-24 address is directly reachable unless the
 *      machine explicitly grants a mapping for it.
 */
static void
machine_protect_low24(void)
{
    mmu_map(
        AMIGA24_BASE,     /* virtual / 68k-visible address */
        AMIGA24_BASE,     /* irrelevant while inaccessible */
        AMIGA24_SIZE,
        0,
        0
    );
}


/*
 * Generic helper for regions that should bypass the fault path.
 *
 * Once a range is mapped here, ordinary CPU accesses become direct
 * MMU-backed loads/stores and DO NOT enter vectors.c or amiga/bus.c.
 */
static void
machine_map_direct_ram(
    uintptr_t guest_addr,
    uintptr_t physical_addr,
    size_t size)
{
    mmu_map(
        guest_addr,
        physical_addr,
        size,
        MMU_NORMAL_LO,
        MMU_NORMAL_HI
    );
}


/*
 * Step 2:
 *
 * Restore only memory we already know is genuine direct memory.
 */
static void
machine_map_known_memory(void)
{
    machine_map_direct_ram(
        CHIPRAM_BASE,
        s_chipram_phys,
        CHIPRAM_SIZE
    );

    /*
     * Nothing else is mapped yet.
     *
     * Later, if observation proves that Slow RAM is required:
     *
     * machine_map_direct_ram(
     *     0x00c00000,
     *     slowram_phys,
     *     0x00080000
     * );
     *
     * And similarly for any other region that is proven to be
     * ordinary memory.
     *
     * Do NOT map:
     *
     *   $DFFxxx       Custom registers
     *   $BFDxxx       CIA-B
     *   $BFExxx       CIA-A
     *
     * Those must remain faulting so the Amiga bus can resolve them.
     *
     * Unknown holes also remain faulting.
     */
}


void
machine_set_chipram(uintptr_t physical_addr)
{
    s_chipram_phys = physical_addr;
}


void
machine_init(void)
{
    /*
     * Start from denial:
     *
     *     low-24 = fault
     */
    machine_protect_low24();

    /*
     * Then grant direct access only to known RAM.
     */
    machine_map_known_memory();

    /*
     * bus/IRQ/Rigel wiring can be initialized separately here
     * once those pieces become part of the scaffold.
     */
}
