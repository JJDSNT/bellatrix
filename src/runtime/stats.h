#ifndef BELLATRIX_RUNTIME_STATS_H
#define BELLATRIX_RUNTIME_STATS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct RuntimeCoreStats {
    const char *name;

    uint64_t steps;
    uint64_t cycles;

    uint64_t max_cycles_per_step;
    uint64_t min_cycles_per_step;

    uint64_t last_cycles_per_step;

    uint64_t events;
    uint64_t interrupts;

    bool initialized;
} RuntimeCoreStats;

typedef struct RuntimeStats {
    RuntimeCoreStats cpu;
    RuntimeCoreStats gfx;
    RuntimeCoreStats audio;
    RuntimeCoreStats io;

    uint64_t total_frames;
    uint64_t total_master_cycles;
} RuntimeStats;

void runtime_stats_init(RuntimeStats *stats);
void runtime_stats_reset(RuntimeStats *stats);

void runtime_stats_step(RuntimeCoreStats *stats,
                        uint64_t cycles);

void runtime_stats_add_event(RuntimeCoreStats *stats);
void runtime_stats_add_interrupt(RuntimeCoreStats *stats);

void runtime_stats_set_total_frames(RuntimeStats *stats,
                                    uint64_t frames);

void runtime_stats_set_master_cycles(RuntimeStats *stats,
                                     uint64_t cycles);

void runtime_stats_dump(const RuntimeStats *stats);

#endif