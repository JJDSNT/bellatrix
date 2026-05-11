#include "runtime/runtime_core.h"

#include <string.h>

void runtime_core_reset(RuntimeCore *core)
{
    if (!core) {
        return;
    }

    core->state = RUNTIME_CORE_STOPPED;
    core->local_cycles = 0;
}

bool runtime_core_start(RuntimeCore *core)
{
    if (!core) {
        return false;
    }

    if (core->init) {
        if (!core->init(core)) {
            return false;
        }
    }

    core->state = RUNTIME_CORE_RUNNING;

    return true;
}

void runtime_core_stop(RuntimeCore *core)
{
    if (!core) {
        return;
    }

    if (core->shutdown) {
        core->shutdown(core);
    }

    core->state = RUNTIME_CORE_STOPPED;
}

void runtime_core_pause(RuntimeCore *core)
{
    if (!core) {
        return;
    }

    if (core->state == RUNTIME_CORE_RUNNING) {
        core->state = RUNTIME_CORE_PAUSED;
    }
}

void runtime_core_resume(RuntimeCore *core)
{
    if (!core) {
        return;
    }

    if (core->state == RUNTIME_CORE_PAUSED) {
        core->state = RUNTIME_CORE_RUNNING;
    }
}

bool runtime_core_is_running(const RuntimeCore *core)
{
    if (!core) {
        return false;
    }

    return core->state == RUNTIME_CORE_RUNNING;
}

void runtime_core_step(RuntimeCore *core,
                       uint32_t cycles)
{
    if (!core) {
        return;
    }

    if (core->state != RUNTIME_CORE_RUNNING) {
        return;
    }

    core->local_cycles += cycles;

    if (core->step) {
        core->step(core, cycles);
    }
}

const char *runtime_core_type_name(
    RuntimeCoreType type)
{
    switch (type) {
    case RUNTIME_CORE_CPU:
        return "CPU";

    case RUNTIME_CORE_GFX:
        return "GFX";

    case RUNTIME_CORE_AUDIO:
        return "AUDIO";

    case RUNTIME_CORE_IO:
        return "IO";

    case RUNTIME_CORE_COUNT:
    default:
        return "UNKNOWN";
    }
}