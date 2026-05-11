#include "runtime/stats.h"

#include <string.h>

#if defined(BELLATRIX)
/* Bare-metal: Emu68 provides kprintf via support.h */
#include "support.h"
#define STATS_PRINTF kprintf
#else
#include <stdio.h>
#define STATS_PRINTF printf
#endif

static void runtime_core_stats_init(RuntimeCoreStats *stats,
                                    const char *name)
{
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    stats->name = name;

    stats->min_cycles_per_step = UINT64_MAX;

    stats->initialized = true;
}

void runtime_stats_init(RuntimeStats *stats)
{
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    runtime_core_stats_init(&stats->cpu, "CPU");
    runtime_core_stats_init(&stats->gfx, "GFX");
    runtime_core_stats_init(&stats->audio, "AUDIO");
    runtime_core_stats_init(&stats->io, "IO");
}

void runtime_stats_reset(RuntimeStats *stats)
{
    runtime_stats_init(stats);
}

void runtime_stats_step(RuntimeCoreStats *stats,
                        uint64_t cycles)
{
    if (!stats || !stats->initialized) {
        return;
    }

    stats->steps++;
    stats->cycles += cycles;

    stats->last_cycles_per_step = cycles;

    if (cycles > stats->max_cycles_per_step) {
        stats->max_cycles_per_step = cycles;
    }

    if (cycles < stats->min_cycles_per_step) {
        stats->min_cycles_per_step = cycles;
    }
}

void runtime_stats_add_event(RuntimeCoreStats *stats)
{
    if (!stats || !stats->initialized) {
        return;
    }

    stats->events++;
}

void runtime_stats_add_interrupt(RuntimeCoreStats *stats)
{
    if (!stats || !stats->initialized) {
        return;
    }

    stats->interrupts++;
}

void runtime_stats_set_total_frames(RuntimeStats *stats,
                                    uint64_t frames)
{
    if (!stats) {
        return;
    }

    stats->total_frames = frames;
}

void runtime_stats_set_master_cycles(RuntimeStats *stats,
                                     uint64_t cycles)
{
    if (!stats) {
        return;
    }

    stats->total_master_cycles = cycles;
}

static void runtime_core_stats_dump(
    const RuntimeCoreStats *stats)
{
    if (!stats || !stats->initialized) {
        return;
    }

    STATS_PRINTF(
        "[RUNTIME-STATS] %-5s "
        "steps=%llu "
        "cycles=%llu "
        "last=%llu "
        "min=%llu "
        "max=%llu "
        "events=%llu "
        "interrupts=%llu\n",

        stats->name,

        (unsigned long long)stats->steps,
        (unsigned long long)stats->cycles,

        (unsigned long long)stats->last_cycles_per_step,

        (unsigned long long)
            (stats->min_cycles_per_step == UINT64_MAX
                 ? 0
                 : stats->min_cycles_per_step),

        (unsigned long long)stats->max_cycles_per_step,

        (unsigned long long)stats->events,

        (unsigned long long)stats->interrupts);
}

void runtime_stats_dump(const RuntimeStats *stats)
{
    if (!stats) {
        return;
    }

    STATS_PRINTF(
        "[RUNTIME-STATS] "
        "frames=%llu "
        "master_cycles=%llu\n",

        (unsigned long long)stats->total_frames,

        (unsigned long long)stats->total_master_cycles);

    runtime_core_stats_dump(&stats->cpu);
    runtime_core_stats_dump(&stats->gfx);
    runtime_core_stats_dump(&stats->audio);
    runtime_core_stats_dump(&stats->io);
}