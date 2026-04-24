// src/debug/cpu_pc.h
//
// Returns the current M68K PC for debug logging.
// Implemented in machine.c; routes through the active CpuBackend.
// Returns 0 if no backend is attached (harness before init, bare builds).

#pragma once
#include <stdint.h>

uint32_t bellatrix_debug_cpu_pc(void);
