#ifndef BELLATRIX_RUNTIME_AFFINITY_H
#define BELLATRIX_RUNTIME_AFFINITY_H

#include <stdbool.h>

typedef enum RuntimeAffinityCore {
    RUNTIME_AFFINITY_CPU     = 0,   /* Core 0 — Emu68 JIT (boot core, fixed) */
    RUNTIME_AFFINITY_CHIPSET = 1,   /* Core 1 — Rigel chipset (full domain)  */
    RUNTIME_AFFINITY_IO      = 3    /* Core 3 — USB / Bluetooth              */
} RuntimeAffinityCore;

bool runtime_affinity_set_current_thread(RuntimeAffinityCore core);
bool runtime_affinity_set_current_thread_id(int core_id);

const char *runtime_affinity_core_name(RuntimeAffinityCore core);

#endif