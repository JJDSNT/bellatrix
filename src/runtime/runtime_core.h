#ifndef BELLATRIX_RUNTIME_RUNTIME_CORE_H
#define BELLATRIX_RUNTIME_RUNTIME_CORE_H

#include <stdint.h>
#include <stdbool.h>

typedef enum RuntimeCoreType {
    RUNTIME_CORE_CPU = 0,
    RUNTIME_CORE_GFX,
    RUNTIME_CORE_AUDIO,
    RUNTIME_CORE_IO,
    RUNTIME_CORE_COUNT
} RuntimeCoreType;

typedef enum RuntimeCoreState {
    RUNTIME_CORE_STOPPED = 0,
    RUNTIME_CORE_RUNNING,
    RUNTIME_CORE_PAUSED
} RuntimeCoreState;

typedef struct RuntimeCore RuntimeCore;

typedef bool (*runtime_core_init_fn)(RuntimeCore *core);
typedef void (*runtime_core_shutdown_fn)(RuntimeCore *core);
typedef void (*runtime_core_step_fn)(RuntimeCore *core,
                                     uint32_t cycles);

struct RuntimeCore {
    RuntimeCoreType type;

    const char *name;

    RuntimeCoreState state;

    /*
     * Future:
     * pthread_t/thread handle
     * affinity
     * mailbox
     * event queue
     */

    void *userdata;

    uint64_t local_cycles;

    runtime_core_init_fn init;
    runtime_core_shutdown_fn shutdown;
    runtime_core_step_fn step;
};

void runtime_core_reset(RuntimeCore *core);

bool runtime_core_start(RuntimeCore *core);

void runtime_core_stop(RuntimeCore *core);

void runtime_core_pause(RuntimeCore *core);

void runtime_core_resume(RuntimeCore *core);

bool runtime_core_is_running(const RuntimeCore *core);

void runtime_core_step(RuntimeCore *core,
                       uint32_t cycles);

const char *runtime_core_type_name(
    RuntimeCoreType type);

#endif