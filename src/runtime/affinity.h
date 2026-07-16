#ifndef BELLATRIX_RUNTIME_AFFINITY_H
#define BELLATRIX_RUNTIME_AFFINITY_H

#include <stdbool.h>

/* POSIX harness affinity is intentionally separate from the bare-metal
 * role-to-core contract in topology.h. These values select host CPUs only. */
typedef enum RuntimeAffinityCore {
    RUNTIME_AFFINITY_CPU     = 0,
    RUNTIME_AFFINITY_CHIPSET = 1,
    RUNTIME_AFFINITY_IO      = 3
} RuntimeAffinityCore;

bool runtime_affinity_set_current_thread(RuntimeAffinityCore core);
bool runtime_affinity_set_current_thread_id(int core_id);

const char *runtime_affinity_core_name(RuntimeAffinityCore core);

#endif
