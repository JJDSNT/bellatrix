#include "cpu/musashi/musashi_bus_timing.h"

#include "cpu/cpu_bridge.h"
#include "machine/machine.h"

#include "m68k.h"

static MusashiBusTiming *s_active_timing;

static void musashi_bus_timing_set_fc(unsigned int function_code)
{
    if (s_active_timing)
        s_active_timing->function_code = function_code;
}

void musashi_bus_timing_init(MusashiBusTiming *timing,
                             const CpuBusPolicy *policy)
{
    if (!timing)
        return;

    timing->policy = policy;
    timing->published_cycles = 0u;
    timing->prepublished_cycles = 0u;
    timing->function_code = 0u;
    timing->run_active = 0;
    s_active_timing = timing;
    m68k_set_fc_callback(musashi_bus_timing_set_fc);
}

void musashi_bus_timing_begin_run(MusashiBusTiming *timing)
{
    if (!timing)
        return;
    timing->published_cycles = 0u;
    timing->run_active = 1;
}

uint32_t musashi_bus_timing_sync(MusashiBusTiming *timing)
{
    int ran;
    uint32_t delta;

    if (!timing || !timing->run_active)
        return 0u;

    ran = m68k_cycles_run();
    if (ran <= 0 || (uint32_t)ran <= timing->published_cycles)
        return 0u;

    delta = (uint32_t)ran - timing->published_cycles;
    timing->published_cycles = (uint32_t)ran;
    if (timing->prepublished_cycles != 0u) {
        uint32_t covered = delta < timing->prepublished_cycles
            ? delta : timing->prepublished_cycles;
        delta -= covered;
        timing->prepublished_cycles -= covered;
    }
    if (delta != 0u)
        bellatrix_bridge_cpu_progress(delta);
    return delta;
}

MusashiBusAccessResult musashi_bus_timing_access(MusashiBusTiming *timing,
                                                 unsigned int size,
                                                 int is_write)
{
    MusashiBusAccessResult result = {0u, 0u};
    const CpuBusPolicy *policy;
    unsigned int transfers;
    uint32_t wait_cpu_cycles;

    result.progress_cycles = musashi_bus_timing_sync(timing);
    if (!timing || !(policy = timing->policy) ||
        !policy->accounts_chip_access)
        return result;

    if (policy->chip_access_program_only &&
        (is_write ||
         (timing->function_code != 2u && timing->function_code != 6u)))
        return result;

    transfers = size == 4u ? 2u : 1u;
    result.waited_cck = bellatrix_machine_cpu_chip_access(
        transfers, 2u, policy->stalls_on_chip_access);
    timing->prepublished_cycles +=
        transfers * policy->chip_transfer_cpu_cycles;

    wait_cpu_cycles = result.waited_cck * 2u;
    if (wait_cpu_cycles != 0u) {
        m68k_modify_timeslice(-(int)wait_cpu_cycles);
        timing->published_cycles += wait_cpu_cycles;
    }
    return result;
}

void musashi_bus_timing_end_run(MusashiBusTiming *timing)
{
    if (!timing)
        return;
    (void)musashi_bus_timing_sync(timing);
    timing->run_active = 0;
}
