#ifndef BELLATRIX_RUNTIME_SYNC_H
#define BELLATRIX_RUNTIME_SYNC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct RuntimeSync {
    bool enabled;

    uint64_t last_master_cycles;
    uint64_t target_master_cycles;

    bool cpu_ready;
    bool gfx_ready;
    bool audio_ready;
    bool io_ready;
} RuntimeSync;

void runtime_sync_init(RuntimeSync *sync);
void runtime_sync_reset(RuntimeSync *sync);

void runtime_sync_enable(RuntimeSync *sync, bool enabled);
bool runtime_sync_is_enabled(const RuntimeSync *sync);

void runtime_sync_begin_slice(RuntimeSync *sync,
                              uint64_t current_master_cycles,
                              uint32_t slice_cycles);

uint64_t runtime_sync_target_cycles(const RuntimeSync *sync);

void runtime_sync_mark_cpu_ready(RuntimeSync *sync);
void runtime_sync_mark_gfx_ready(RuntimeSync *sync);
void runtime_sync_mark_audio_ready(RuntimeSync *sync);
void runtime_sync_mark_io_ready(RuntimeSync *sync);

bool runtime_sync_all_ready(const RuntimeSync *sync);

void runtime_sync_clear_ready(RuntimeSync *sync);

#endif