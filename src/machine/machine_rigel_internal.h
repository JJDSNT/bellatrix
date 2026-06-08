/* src/machine/machine_rigel_internal.h
 *
 * Cross-file declarations for the machine_rigel split.
 * Every machine_rigel_*.c TU includes this header.
 *
 * Globals that belong entirely to one sub-file (g_rtrace, g_rigel_cia_trace,
 * g_rigel_floppy_trace, s_cpu_approx, s_cpu_cck_rem, s_quantum) stay static
 * in their respective file and are NOT declared here.
 */

#ifndef MACHINE_RIGEL_INTERNAL_H
#define MACHINE_RIGEL_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "machine/machine.h"
#include "rigel/rigel.h"

/* ---------------------------------------------------------------------------
 * Globals defined in machine_rigel.c
 * ------------------------------------------------------------------------- */

/* Actual M68K PC captured in vectors.c before each bellatrix_bus_access call. */
extern volatile uint32_t g_bellatrix_fault_pc;

/* Most recent M68K PC captured in ExecutionLoop.c before machine_advance. */
extern volatile uint32_t g_bellatrix_exec_pc;

extern BellatrixMachine  g_machine;
extern RigelContext      *g_rigel;
extern bool               g_rigel_zero_copy_video;

/* Bare-metal framebuffer — declared by the platform (VC4 / host SDL layer). */
extern uint16_t *framebuffer;
extern uint32_t  pitch;
extern uint32_t  fb_width;
extern uint32_t  fb_height;

/* ---------------------------------------------------------------------------
 * Cross-file function declarations
 * (static removed; each function is defined in its respective .c file)
 * ------------------------------------------------------------------------- */

/* --- machine_rigel.c (lifecycle / debug helpers) --- */
uint32_t bellatrix_debug_cpu_pc(void);

/* --- machine_rigel_trace.c --- */
void machine_rigel_trace_init(void);
void machine_rigel_trace_step(const rigel_step_result_t *r);
int  machine_rigel_cia_trace_enabled(void);
int  machine_rigel_floppy_trace_enabled(void);
void machine_rigel_trace_floppy(const char *reason, uint32_t pc,
                                uint8_t reg, uint8_t value);
int  machine_rigel_blitter_trace_enabled(void);
bool machine_rigel_rtrace_enabled(void);   /* returns g_rtrace.enabled */

/* --- machine_rigel_trace.c (IPL publish lives here, uses g_rtrace) --- */
void machine_publish_ipl(BellatrixMachine *m, uint8_t ipl);

/* --- machine_rigel_trace.c (log callbacks passed to rigel_create) --- */
void machine_rigel_log(const char *msg, void *opaque);
void machine_rigel_log_event(const rigel_log_event_t *event, void *opaque);

/* --- machine_rigel_bus.c --- */
uint16_t rigel_chip_ram_read16(void *opaque, uint32_t addr);
void     rigel_chip_ram_write16(void *opaque, uint32_t addr, uint16_t value);
uint32_t machine_dispatch_read(BellatrixMachine *m, uint32_t addr,
                                unsigned int size);
void     machine_dispatch_write(BellatrixMachine *m, uint32_t addr,
                                 uint32_t value, unsigned int size);

/* --- machine_rigel_step.c --- */
rigel_cycle_t machine_next_quantum(void);
void machine_step_components(BellatrixMachine *m, uint32_t approx);
void machine_flush_for_bus(BellatrixMachine *m);
void machine_present_frame_from_rigel(void);
void machine_step_host_serial_rigel(void);
void machine_drain_serial_fallback_rigel(void);
void machine_sync_controller_ports_rigel(BellatrixMachine *m);
void bellatrix_machine_sync_ipl(void);

/* Quantum constants — shared between step and bus TUs */
#define RIGEL_MAX_QUANTUM  71424u   /* 313 lines × 228 cycles — one PAL frame */
#define RIGEL_MIN_QUANTUM      8u   /* never step by zero                      */

/* Step accumulators — defined in machine_rigel_step.c.
 * Exported here because bellatrix_machine_read/write (in bus TU) must
 * touch them inline to account for the bus cycle before flushing. */
extern uint64_t      s_cpu_approx;
extern uint32_t      s_cpu_cck_rem;
extern rigel_cycle_t s_quantum;

#endif /* MACHINE_RIGEL_INTERNAL_H */
