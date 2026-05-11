#ifndef BELLATRIX_RUNTIME_AFFINITY_H
#define BELLATRIX_RUNTIME_AFFINITY_H

#include <stdbool.h>

typedef enum RuntimeAffinityCore {
    RUNTIME_AFFINITY_CPU   = 0,   /* Core 0 — Emu68 JIT (boot core, fixed) */
    RUNTIME_AFFINITY_GFX   = 1,   /* Core 1 — Agnus / DMA / Denise         */
    RUNTIME_AFFINITY_AUDIO = 2,   /* Core 2 — Paula audio                  */
    RUNTIME_AFFINITY_IO    = 3    /* Core 3 — CIA / serial / disk           */
} RuntimeAffinityCore;

bool runtime_affinity_set_current_thread(RuntimeAffinityCore core);
bool runtime_affinity_set_current_thread_id(int core_id);

const char *runtime_affinity_core_name(RuntimeAffinityCore core);

#endif