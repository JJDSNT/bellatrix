#include "runtime/affinity.h"

#if defined(__linux__) && !defined(BELLATRIX)
#define _GNU_SOURCE
#include <sched.h>
#include <unistd.h>
#endif

const char *runtime_affinity_core_name(RuntimeAffinityCore core)
{
    switch (core) {
    case RUNTIME_AFFINITY_CPU:
        return "CPU";

    case RUNTIME_AFFINITY_GFX:
        return "GFX";

    case RUNTIME_AFFINITY_AUDIO:
        return "AUDIO";

    case RUNTIME_AFFINITY_IO:
        return "IO";

    default:
        return "UNKNOWN";
    }
}

bool runtime_affinity_set_current_thread(RuntimeAffinityCore core)
{
    return runtime_affinity_set_current_thread_id((int)core);
}

bool runtime_affinity_set_current_thread_id(int core_id)
{
    if (core_id < 0) {
        return false;
    }

#if defined(__linux__) && !defined(BELLATRIX)
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count <= 0 || core_id >= cpu_count) {
        return false;
    }

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core_id, &set);

    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    (void)core_id;
    return false;
#endif
}