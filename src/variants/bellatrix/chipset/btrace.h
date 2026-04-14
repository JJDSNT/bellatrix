// src/variants/bellatrix/chipset/btrace.h

#ifndef _BELLATRIX_BTRACE_H
#define _BELLATRIX_BTRACE_H

#include <stdint.h>

#define BUS_READ  0
#define BUS_WRITE 1

// Verbosity filter flags (written via 0xDFFF00).
#define BTRACE_OFF      0x0000
#define BTRACE_UNIMPL   0x0001
#define BTRACE_CIA      0x0002
#define BTRACE_CHIPSET  0x0004
#define BTRACE_ALL      0xFFFF

// Ring buffer size for watchdog post-mortem dump.
#define BTRACE_RING_SIZE 64

void btrace_init(void);

// Log a single bus access. Called from bellatrix_bus_access().
// impl=1 if a handler exists for addr, 0 if unimplemented.
void btrace_log(uint32_t addr, uint32_t value, int size, int dir, int impl);

// Set verbosity filter (called when 0xDFFF00 is written).
void btrace_set_filter(uint16_t filter);

// Dump ring buffer to UART as a watchdog post-mortem.
void btrace_dump_ring(void);

// Called each VBL tick to decrement the watchdog counter.
void btrace_watchdog_tick(void);

#endif /* _BELLATRIX_BTRACE_H */
