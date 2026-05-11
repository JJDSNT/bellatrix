#include "runtime/runtime.h"

#include <string.h>

#include "debug/core_log.h"
#include "support.h"

bool bellatrix_runtime_init(
    BellatrixRuntime *rt,
    BellatrixMachine *machine)
{
    if (!rt || !machine) {
        return false;
    }

    memset(rt, 0, sizeof(*rt));

    rt->machine = machine;

    /*
     * Conservative default.
     * Can later become dynamic or beam-based.
     */
    rt->cycles_per_step = 4;

    if (!core_cpu_init(&rt->cpu, machine)) {
        return false;
    }

    if (!core_gfx_init(&rt->gfx, machine)) {
        return false;
    }

    if (!core_audio_init(&rt->audio, machine)) {
        return false;
    }

    if (!core_io_init(&rt->io, machine)) {
        return false;
    }

    rt->running = true;

    kprintf("[RUNTIME] init OK: Core0=CPU Core1=GFX Core2=PAULA Core3=IO\n");
    return true;
}

void bellatrix_runtime_shutdown(
    BellatrixRuntime *rt)
{
    if (!rt) {
        return;
    }

    kprintf("[RUNTIME] shutdown\n");
    core_cpu_shutdown(&rt->cpu);
    core_gfx_shutdown(&rt->gfx);
    core_audio_shutdown(&rt->audio);
    core_io_shutdown(&rt->io);

    rt->running = false;
}

void bellatrix_runtime_reset(
    BellatrixRuntime *rt)
{
    if (!rt || !rt->machine) {
        return;
    }

    kprintf("[RUNTIME] reset\n");
    bellatrix_machine_reset();

    rt->cpu.local_cycles = 0;

    rt->gfx.master_cycles = 0;
    rt->gfx.frame_counter = 0;

    rt->audio.local_cycles = 0;

    rt->io.local_cycles = 0;
}

void bellatrix_runtime_step(
    BellatrixRuntime *rt)
{
    if (!rt || !rt->running) {
        return;
    }

    uint32_t cycles = rt->cycles_per_step;

    /*
     * -----------------------------------------------------------------
     * GFX FIRST
     * -----------------------------------------------------------------
     *
     * Agnus owns observable time.
     *
     * Everything else consumes that time.
     */
    core_gfx_step(&rt->gfx, cycles);

    /*
     * -----------------------------------------------------------------
     * CPU
     * -----------------------------------------------------------------
     */
    core_cpu_step(&rt->cpu, cycles);

    /*
     * -----------------------------------------------------------------
     * AUDIO
     * -----------------------------------------------------------------
     *
     * Audio follows Agnus master time.
     */
    core_audio_step(
        &rt->audio,
        core_gfx_master_cycles(&rt->gfx));

    /*
     * -----------------------------------------------------------------
     * IO
     * -----------------------------------------------------------------
     */
    core_io_step(&rt->io, cycles);
}

void bellatrix_runtime_run(
    BellatrixRuntime *rt)
{
    if (!rt) {
        return;
    }

    rt->running = true;

    while (rt->running) {
        bellatrix_runtime_step(rt);
    }
}

void bellatrix_runtime_stop(
    BellatrixRuntime *rt)
{
    if (!rt) {
        return;
    }

    rt->running = false;
}