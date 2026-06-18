// src/cpu/cpu_bridge.h
//
// Shared CPU <-> machine bridge.

#pragma once

#include <stdint.h>

enum {
    BELLATRIX_BUS_READ = 0,
    BELLATRIX_BUS_WRITE = 1,
};

uint32_t bellatrix_bridge_normalize_addr(uint32_t addr);

uint32_t bellatrix_bridge_cpu_read(uint32_t addr, unsigned int size);
void     bellatrix_bridge_cpu_write(uint32_t addr, uint32_t value, unsigned int size);

uint32_t bellatrix_bridge_cpu_access(uint32_t addr,
                                     uint32_t value,
                                     unsigned int size,
                                     int dir);

void bellatrix_bridge_publish_cpu_cycles(uint32_t cycles);
void bellatrix_bridge_cpu_progress(uint32_t cycles);
void bellatrix_bridge_cpu_sync_ipl(void);
