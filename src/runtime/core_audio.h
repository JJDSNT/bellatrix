#ifndef BELLATRIX_RUNTIME_CORE_AUDIO_H
#define BELLATRIX_RUNTIME_CORE_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#include "core/machine.h"

#include "runtime/runtime_core.h"

typedef struct RuntimeCoreAudio {
    RuntimeCore base;

    BellatrixMachine *machine;

    /*
     * Audio follows Agnus master time.
     */
    uint64_t last_master_cycles;

    uint64_t local_cycles;

    bool enabled;
} RuntimeCoreAudio;

bool core_audio_init(RuntimeCoreAudio *core,
                     BellatrixMachine *machine);

void core_audio_shutdown(RuntimeCoreAudio *core);

void core_audio_reset(RuntimeCoreAudio *core);

void core_audio_step(RuntimeCoreAudio *core,
                     uint64_t target_master_cycles);

#endif