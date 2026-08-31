/*
 * src/machine/vecpage.c
 *
 * Catching whoever writes the first page of chip RAM, with their exact PC.
 *
 * AROS m68k links SysBase and AbsExecBase as absolute address 4
 * (arch/m68k-emu68/boot/mmakefile.src: `--defsym,SysBase=0x4`), so the
 * longword at chip RAM offset 4 is what every `moveal SysBase,%a6` in the
 * kernel reads. Under ISSUE-0082 it changes while DPaint has an Amiga screen
 * open, and the machine then dies at whatever library call comes next.
 *
 * That write cannot be seen where it happens. Chip RAM is MACHINE_REGION_DIRECT
 * (machine.c), so the CPU writes it with native stores that reach no hook --
 * and the chipset-side guard in amiga_chip_ram_write16() stayed silent because
 * the write is not chipset DMA either.
 *
 * The legacy tree had already solved this and the answer is not a new idea:
 *
 *     Diagnostic (opt-in): fault-drive the vector page so every access to
 *     0x000-0xFFF reaches bellatrix_bus_access and low-memory corruption
 *     writers are caught with their exact PC ([VEC-W]). ~15x boot slowdown
 *     under QEMU/TCG -- enable only for targeted hunts.
 *
 * It also recorded why the page is direct the rest of the time, which is not a
 * matter of taste: legacy's own note says a write-trap on pages 0-1 produced
 * store-buffer coherency failures between the EL1 alias and the guest's low
 * mapping, "for programs testing $000400".
 *
 * So this is opt-in, by boot argument, and off by default. Turning it on
 * costs a fault on every access to the vector page -- which includes every
 * library call in the system reading AbsExecBase -- and buys the one thing the
 * crash report cannot give: the address, the value and the PC of the write
 * itself, rather than the name of the first call that tripped over it.
 */

#include "machine/vecpage.h"

#include "machine/region.h"

#include "A64.h"
#include "M68k.h"
#include "support.h"

/*
 * Emu68 keeps a cached identity map of physical memory at this offset
 * (src/aarch64/mmu.c: PHYS_VIRT_OFFSET), and its own fault path already
 * services trapped accesses through it (vectors.c: SYSWriteValToAddr). Using
 * the same window here is what lets these ops touch a page that is trapped for
 * everyone else without faulting recursively.
 */
#define PHYS_WINDOW 0xffffff9000000000ULL

extern struct M68KState *__m68k_state;

static int vecpage_requested;

void bellatrix_parse_cmdline(const char *cmdline)
{
    if (cmdline == 0)
        return;

    vecpage_requested = find_token(cmdline, "bellatrix.vecpage") != 0;

    if (vecpage_requested)
        kprintf("[BELLATRIX:VECPAGE] armed by the command line:"
                " the first page of chip RAM is fault-driven\n");
}

int machine_vecpage_trapped(void)
{
    return vecpage_requested;
}

static uint32_t vecpage_read(const MachineRegion *region, uint32_t address,
                             int size)
{
    (void)region;

    switch (size)
    {
        case 1: return *(volatile const uint8_t  *)(PHYS_WINDOW + address);
        case 2: return *(volatile const uint16_t *)(PHYS_WINDOW + address);
        default: return *(volatile const uint32_t *)(PHYS_WINDOW + address);
    }
}

static void vecpage_write(const MachineRegion *region, uint32_t address,
                          int size, uint32_t value)
{
    (void)region;

    /*
     * What gets a budget, and what must never have one.
     *
     * The first version of this capped every write at 32 reports, and that was
     * the same mistake this project has now paid for three times: AROS fills
     * the 68k vector table at startup, which is ~64 longwords, so the budget
     * was gone before the interesting window began. A run then produced no
     * [VEC-W] lines at all -- and a trap that was never armed produces exactly
     * the same log, so the result said nothing either way. CLAUDE.md states
     * the rule outright ("a probe that prints nothing and a probe that never
     * ran look identical") and ISSUE-0078 records it as the most expensive
     * trap in the dwc2 driver. It applies to new instruments too.
     *
     * So AbsExecBase is uncapped. There are two writes to it in a whole boot:
     * exec installing itself, and the one this issue is about. Everything else
     * shares a small budget, which is enough to characterise the traffic
     * without burying those two.
     *
     * The PC is the datum this instrument exists for. It comes out of the
     * JIT's saved context, which is the last address the CPU left translated
     * code at -- and for the fault taken *by* this store, that is the store.
     *
     * Reads are deliberately not reported at all: 3011 sites in the kernel
     * read AbsExecBase, and they would bury everything.
     */
    {
        static unsigned reported;
        int absexec = (address < 8u && address + (uint32_t)size > 4u);

        if (absexec || reported < 8u)
        {
            if (!absexec)
                reported++;

            kprintf("[VEC-W%d] addr=%06x value=%08x pc=%08x sr=%04x%s\n",
                    size * 8, (unsigned)address, (unsigned)value,
                    (unsigned)(__m68k_state != 0 ? __m68k_state->PC : 0),
                    (unsigned)(__m68k_state != 0 ? __m68k_state->SR : 0),
                    absexec ? "  <- AbsExecBase" : "");
        }
    }

    switch (size)
    {
        case 1: *(volatile uint8_t  *)(PHYS_WINDOW + address) = (uint8_t)value;  break;
        case 2: *(volatile uint16_t *)(PHYS_WINDOW + address) = (uint16_t)value; break;
        default: *(volatile uint32_t *)(PHYS_WINDOW + address) = value;          break;
    }
}

const MachineRegionOps machine_vecpage_ops =
{
    .read  = vecpage_read,
    .write = vecpage_write,
};
