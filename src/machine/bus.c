/*
 * src/machine/bus.c
 *
 * Who touches the classic address space, and where from.
 *
 * The low 24-bit domain is trapped by machine policy (machine.c), so every
 * access to it arrives at Emu68's fault path. This is the discovery phase:
 * before any range is classified as RAM, MMIO, a mirror or open bus, the
 * machine has to know what actually reaches it and which guest code sent it.
 *
 * Nothing here handles a transaction. Emu68's existing behaviour is left
 * untouched -- an observed access is still serviced exactly as before, so this
 * changes what is known, not what happens. See docs/New_emu68.md section 9 and
 * AI_context/issues/ISSUE-0016.md.
 */

#include "machine/bus.h"

#include <stdint.h>

#include "A64.h"
#include "M68k.h"

#define BUS24_LIMIT         0x01000000UL

/*
 * Report each distinct (pc, address, direction) once.
 *
 * A raw log is not usable here and the reason is measured, not feared: a guest
 * sweeping the address space produced 262K faults in one boot on this project
 * before, and the serial console became the bottleneck rather than the
 * instrument. What identifies the access is the pair (guest PC, address); how
 * often it repeats is a count, not a line.
 */
#define BUS24_TABLE_ENTRIES 192

struct bus24_entry
{
    uint32_t pc;
    uint32_t address;
    uint32_t count;
    uint8_t  size;
    uint8_t  write;
    uint8_t  used;
};

static struct bus24_entry bus24_table[BUS24_TABLE_ENTRIES];
static uint32_t bus24_distinct;
static uint32_t bus24_total;
static uint8_t  bus24_table_full_reported;

void machine_bus_observe(uint32_t address, int size, int write)
{
    struct M68KState *m68k;
    uint64_t x18_save;
    uint64_t v30_save;
    uint32_t pc;
    unsigned int i;

    if (address >= BUS24_LIMIT)
        return;

    bus24_total++;

    /*
     * Emu68 keeps the m68k context in a reserved register, which is how the
     * guest PC is recovered here; the AROS image loads at a known base, so the
     * address resolves against the ELF this repository builds.
     */
    __asm__ volatile("mov %0, "CTX_POINTER_ASM"\n" : "=r"(m68k));
    pc = m68k ? m68k->PC : 0;

    for (i = 0; i < BUS24_TABLE_ENTRIES; i++)
    {
        struct bus24_entry *e = &bus24_table[i];

        if (!e->used)
            break;

        if (e->pc == pc && e->address == address &&
            e->write == (uint8_t)!!write && e->size == (uint8_t)size)
        {
            e->count++;
            return;
        }
    }

    /*
     * kprintf clobbers the pinned m68k PC in x18 and the modeled-cycle counter
     * in v30. This is not defensive coding: the legacy integration lost boots
     * to exactly that, because the instrument was corrupting the state it was
     * there to observe. Preserve both across every call that can print.
     */
    __asm__ volatile("mov %0, x18" : "=r"(x18_save));
    __asm__ volatile("mov %0, v30.d[0]" : "=r"(v30_save));

    if (i == BUS24_TABLE_ENTRIES)
    {
        if (!bus24_table_full_reported)
        {
            bus24_table_full_reported = 1;
            kprintf("[BELLATRIX] bus24: table full at %d distinct accesses -- "
                    "further ones are counted in the total only\n",
                    bus24_distinct);
        }
    }
    else
    {
        struct bus24_entry *e = &bus24_table[i];

        e->pc = pc;
        e->address = address;
        e->size = (uint8_t)size;
        e->write = (uint8_t)!!write;
        e->count = 1;
        e->used = 1;
        bus24_distinct++;

        kprintf("[BELLATRIX] bus24 %s%-2d addr=%08x pc=%08x (#%d)\n",
                write ? "W" : "R", size * 8, address, pc, bus24_distinct);
    }

    __asm__ volatile("mov v30.d[0], %0" :: "r"(v30_save));
    __asm__ volatile("mov x18, %0" :: "r"(x18_save));
}

void machine_bus_report(void)
{
    unsigned int i;

    kprintf("[BELLATRIX] bus24 summary: %d accesses, %d distinct\n",
            bus24_total, bus24_distinct);

    for (i = 0; i < BUS24_TABLE_ENTRIES && bus24_table[i].used; i++)
    {
        const struct bus24_entry *e = &bus24_table[i];

        kprintf("[BELLATRIX] bus24   %s%-2d addr=%08x pc=%08x count=%d\n",
                e->write ? "W" : "R", e->size * 8, e->address, e->pc,
                e->count);
    }
}
