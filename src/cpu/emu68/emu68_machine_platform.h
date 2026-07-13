#ifndef EMU68_MACHINE_PLATFORM_H
#define EMU68_MACHINE_PLATFORM_H

#include "cpu/emu68/emu68_machine.h"

emu68_status_t emu68_machine_platform_map_direct(
    uint32_t guest_base, uint64_t size, void *host_base, uint32_t flags);
void emu68_machine_platform_unmap_direct(uint32_t guest_base, uint64_t size);
void emu68_machine_platform_invalidate(uint32_t guest_base, uint64_t size);
void emu68_machine_platform_invalidate_all(void);
emu68_status_t emu68_machine_platform_reset(uint32_t initial_ssp,
                                            uint32_t initial_pc);
emu68_status_t emu68_machine_platform_set_ipl(unsigned level);
void emu68_machine_platform_wake(void);

#endif
