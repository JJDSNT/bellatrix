#ifndef BELLATRIX_RUNTIME_CPU_PROGRESS_H
#define BELLATRIX_RUNTIME_CPU_PROGRESS_H

#include <stdint.h>

/*
 * Runtime-facing CPU cycle publisher.
 *
 * Generic CPU backends use this to hand executed M68K cycles to the active
 * runtime. The implementation may publish to another core on bare metal or
 * advance synchronously in the host harness.
 */
void bellatrix_runtime_publish_cpu_cycles(uint32_t cycles);

#endif
