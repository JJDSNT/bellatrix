#include "debug/debug.h"

#include "core/machine.h"
#include "debug/probe.h"
#include "debug/os_debug.h"
#include "debug/emu_debug.h"
#include "support.h"

void bellatrix_debug_dump_probe(BellatrixMachine *m, uint32_t last_n)
{
    if (!m) {
        return;
    }

    kprintf("\n");
    kprintf("############################################################\n");
    kprintf("# Bellatrix debug: probe\n");
    kprintf("############################################################\n");

    probe_dump(&m->debug.probe, last_n);
}

void bellatrix_debug_dump_all(BellatrixMachine *m,
                              uint32_t probe_last_n,
                              uint32_t copper_max_insn)
{
    if (!m) {
        return;
    }

    kprintf("\n");
    kprintf("############################################################\n");
    kprintf("# Bellatrix debug dump\n");
    kprintf("############################################################\n");
    kprintf("[DBG] tick_count=%08x current_ipl=%u\n",
            (uint32_t)m->tick_count,
            (unsigned)m->current_ipl);

    probe_dump(&m->debug.probe, probe_last_n);
    os_debug_dump(m);
    emu_debug_dma(m);
    emu_debug_copper(m, copper_max_insn);
}

void bellatrix_debug_dump(BellatrixMachine *m)
{
    bellatrix_debug_dump_all(m, 128, 32);
}