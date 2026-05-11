#include "runtime/sync.h"

#include <string.h>

void runtime_sync_init(RuntimeSync *sync)
{
    if (!sync) {
        return;
    }

    memset(sync, 0, sizeof(*sync));
    sync->enabled = true;
}

void runtime_sync_reset(RuntimeSync *sync)
{
    runtime_sync_init(sync);
}

void runtime_sync_enable(RuntimeSync *sync, bool enabled)
{
    if (!sync) {
        return;
    }

    sync->enabled = enabled;
}

bool runtime_sync_is_enabled(const RuntimeSync *sync)
{
    return sync && sync->enabled;
}

void runtime_sync_begin_slice(RuntimeSync *sync,
                              uint64_t current_master_cycles,
                              uint32_t slice_cycles)
{
    if (!sync) {
        return;
    }

    sync->last_master_cycles = current_master_cycles;
    sync->target_master_cycles =
        current_master_cycles + (uint64_t)slice_cycles;

    runtime_sync_clear_ready(sync);
}

uint64_t runtime_sync_target_cycles(const RuntimeSync *sync)
{
    if (!sync) {
        return 0;
    }

    return sync->target_master_cycles;
}

void runtime_sync_mark_cpu_ready(RuntimeSync *sync)
{
    if (sync) {
        sync->cpu_ready = true;
    }
}

void runtime_sync_mark_gfx_ready(RuntimeSync *sync)
{
    if (sync) {
        sync->gfx_ready = true;
    }
}

void runtime_sync_mark_audio_ready(RuntimeSync *sync)
{
    if (sync) {
        sync->audio_ready = true;
    }
}

void runtime_sync_mark_io_ready(RuntimeSync *sync)
{
    if (sync) {
        sync->io_ready = true;
    }
}

bool runtime_sync_all_ready(const RuntimeSync *sync)
{
    if (!sync) {
        return false;
    }

    if (!sync->enabled) {
        return true;
    }

    return sync->cpu_ready &&
           sync->gfx_ready &&
           sync->audio_ready &&
           sync->io_ready;
}

void runtime_sync_clear_ready(RuntimeSync *sync)
{
    if (!sync) {
        return;
    }

    sync->cpu_ready = false;
    sync->gfx_ready = false;
    sync->audio_ready = false;
    sync->io_ready = false;
}