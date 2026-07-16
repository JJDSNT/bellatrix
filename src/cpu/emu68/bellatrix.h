// src/variants/bellatrix/bellatrix.h
//
// Bellatrix environment interface used by CPU backends and, until the
// independent physical-infrastructure removal, the legacy Emu68 fault path.

#ifndef _BELLATRIX_H
#define _BELLATRIX_H

#include <stdint.h>

#define BUS_READ  0
#define BUS_WRITE 1

// Called once at startup before M68K_StartEmu().
void bellatrix_init(void);
int  bellatrix_cpu_backend_owns_execution_loop(void);
void bellatrix_run_selected_cpu_backend(void);
void bellatrix_emu68_boards_reset(void);

void bellatrix_machine_advance_cpu_cycles(uint32_t cycles);
void bellatrix_emu68_publish_idle_cycles(uint32_t cycles);
void bellatrix_emu68_publish_cpu_progress(uint64_t cycles,
                                         uint64_t instructions,
                                         uint32_t pc);

// Every CPU backend runs on the boot core. Backend selection does not change
// the multicore topology defined by runtime/topology.h.
void bellatrix_launch_cpu_and_park(void (*entry)(void));

// Sync the host MMU overlay mapping with the logical CIA-A PRA OVL bit.
// This is the live path used by Emu68's fault bridge after CIA writes.
void bellatrix_sync_overlay_from_ciaa(void);

// Reset vectors captured by bellatrix_init() directly from the ROM ARM
// address — stable before any Emu68 JIT/cache init runs.
// Both are M68K-side values (big-endian already decoded to host uint32_t).
extern uint32_t bellatrix_reset_isp;
extern uint32_t bellatrix_reset_pc;

// Called from SYSWriteValToAddr / SYSReadValFromAddr for every
// access to an unmapped Amiga address (chipset, CIA, slow RAM, etc.).
//
//   addr  — 24-bit Amiga bus address (already normalised by Emu68)
//   value — data to write (BUS_WRITE) or 0 (BUS_READ)
//   size  — access width in bytes: 1, 2, 4, 8, or 16
//   dir   — BUS_READ or BUS_WRITE
//
// Returns the value to load into the destination register (BUS_READ),
// or 0 (BUS_WRITE).
uint32_t bellatrix_bus_access(uint32_t addr, uint32_t value, int size, int dir);

// Called by the Emu68 JIT integration whenever the current v30 instruction
// counter is observable (JIT loop boundary or MMIO fault handler). Bellatrix
// owns the delta calculation and CPU→chipset progress notification.
void bellatrix_emu68_report_jit_progress(uint64_t insn_count, uint32_t pc);

// Actual M68K PC (x18) captured by SYSWriteValToAddr/SYSReadValFromAddr
// immediately before calling bellatrix_bus_access.  More accurate than
// __m68k_state->PC which is only updated at JIT cache misses.
extern volatile uint32_t g_bellatrix_fault_pc;

// Most recent M68K PC (x18) captured by the ExecutionLoop cycle-ownership
// block just before bellatrix_machine_advance().  Updated each MainLoop
// iteration; useful for VBL and other periodic logs.
extern volatile uint32_t g_bellatrix_exec_pc;

#endif /* _BELLATRIX_H */
