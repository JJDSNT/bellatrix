#ifndef BELLATRIX_RUNTIME_TOPOLOGY_H
#define BELLATRIX_RUNTIME_TOPOLOGY_H

/*
 * Canonical bare-metal role-to-core topology.
 *
 * Core numbers are an implementation detail; runtime code should reason in
 * terms of these roles. Emu68 and Musashi deliberately share the same
 * placement; selecting a CPU backend must not rearrange the machine.
 *
 * Unified topology:
 *   Core 0  selected CPU / physical IRQ ingress
 *   Core 1  auxiliary / parked
 *   Core 2  Rigel chipset
 *   Core 3  host reactor (physical IO, timeline, presentation)
 */

#define BELLATRIX_CORE_BOOT        0u
#define BELLATRIX_CORE_IRQ_INGRESS BELLATRIX_CORE_BOOT
#define BELLATRIX_CORE_CPU         BELLATRIX_CORE_BOOT
#define BELLATRIX_CORE_CHIPSET     2u
#define BELLATRIX_CORE_HOST_REACTOR 3u
#define BELLATRIX_CORE_AUXILIARY    1u

#if defined(BELLATRIX_EMU68_CORE0_REBASELINE) && \
    BELLATRIX_EMU68_CORE0_REBASELINE
#define BELLATRIX_NATIVE_EMU68_INTEGRATION 1
#else
#define BELLATRIX_NATIVE_EMU68_INTEGRATION 0
#endif

#if BELLATRIX_CORE_CPU == BELLATRIX_CORE_CHIPSET
#error "Bellatrix CPU and chipset roles cannot share a core in multicore mode"
#endif

#if BELLATRIX_CORE_HOST_REACTOR == BELLATRIX_CORE_CHIPSET
#error "Bellatrix host reactor and chipset roles cannot share a core"
#endif

#endif
