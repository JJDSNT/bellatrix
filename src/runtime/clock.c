#include "runtime/clock.h"

#include <string.h>

void runtime_clock_init(RuntimeClock *clock)
{
    if (!clock) {
        return;
    }

    memset(clock, 0, sizeof(*clock));
}

void runtime_clock_reset(RuntimeClock *clock)
{
    runtime_clock_init(clock);
}

void runtime_clock_publish(RuntimeClock *clock,
                           uint64_t master_cycles,
                           uint16_t vpos,
                           uint16_t hpos,
                           bool vblank)
{
    if (!clock) {
        return;
    }

    /*
     * Only core_gfx/Agnus should call this.
     */
    clock->master_cycles = master_cycles;
    clock->vpos = vpos;
    clock->hpos = hpos;
    clock->vblank = vblank;
}

uint64_t runtime_clock_cycles(const RuntimeClock *clock)
{
    if (!clock) {
        return 0;
    }

    return clock->master_cycles;
}

uint64_t runtime_clock_frame_counter(const RuntimeClock *clock)
{
    if (!clock) {
        return 0;
    }

    return clock->frame_counter;
}

uint16_t runtime_clock_vpos(const RuntimeClock *clock)
{
    if (!clock) {
        return 0;
    }

    return clock->vpos;
}

uint16_t runtime_clock_hpos(const RuntimeClock *clock)
{
    if (!clock) {
        return 0;
    }

    return clock->hpos;
}

bool runtime_clock_in_vblank(const RuntimeClock *clock)
{
    if (!clock) {
        return false;
    }

    return clock->vblank;
}

void runtime_clock_increment_frame(RuntimeClock *clock)
{
    if (!clock) {
        return;
    }

    clock->frame_counter++;
}