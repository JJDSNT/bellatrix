#ifndef BELLATRIX_RUNTIME_CLOCK_H
#define BELLATRIX_RUNTIME_CLOCK_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Runtime clock policy:
 *
 * Agnus/GFX owns observable emulated time.
 * This clock object does NOT generate time by itself.
 * It only publishes/snapshots the master cycle count advanced by core_gfx.
 */

typedef struct RuntimeClock {
    uint64_t master_cycles;
    uint64_t frame_counter;

    uint16_t vpos;
    uint16_t hpos;

    bool vblank;
} RuntimeClock;

void runtime_clock_init(RuntimeClock *clock);
void runtime_clock_reset(RuntimeClock *clock);

void runtime_clock_publish(RuntimeClock *clock,
                           uint64_t master_cycles,
                           uint16_t vpos,
                           uint16_t hpos,
                           bool vblank);

uint64_t runtime_clock_cycles(const RuntimeClock *clock);
uint64_t runtime_clock_frame_counter(const RuntimeClock *clock);

uint16_t runtime_clock_vpos(const RuntimeClock *clock);
uint16_t runtime_clock_hpos(const RuntimeClock *clock);

bool runtime_clock_in_vblank(const RuntimeClock *clock);

void runtime_clock_increment_frame(RuntimeClock *clock);

#endif