#include "runtime/core_audio.h"

#include <string.h>

#include "chipset/paula/paula_audio.h"
#include "chipset/paula/paula_disk.h"
#include "chipset/paula/paula_serial.h"
#include "chipset/paula/paula_interrupt.h"
#include "machine/machine.h"
#include "debug/core_log.h"

/*
 * Core 2 — Paula domain.
 *
 * Owns: audio DMA, disk Paula (DSKBYTR/DSKLEN), serial Paula (SERDAT/SERDATR),
 *       INTREQ/INTENA consolidation, IPL publication to CPU.
 *
 * Does NOT own: physical floppy drive, host UART, CIA, keyboard.
 * Those live in Core 3 (core_io).
 */

static void core_audio_step_paula_audio(RuntimeCoreAudio *core, uint32_t cycles)
{
    paula_audio_step(&core->machine->paula.audio, cycles);
}

static void core_audio_step_disk(RuntimeCoreAudio *core, uint32_t cycles)
{
    paula_disk_step(&core->machine->paula.disk, cycles);
}

static void core_audio_step_serial(RuntimeCoreAudio *core, uint32_t cycles)
{
    paula_serial_step(&core->machine->paula.serial, cycles);
}

static void core_audio_step_interrupts(RuntimeCoreAudio *core)
{
    uint8_t ipl_before = (uint8_t)core->machine->current_ipl;

    paula_interrupt_update(&core->machine->paula.irq);

    /*
     * Publish the computed IPL to the CPU backend.
     * bellatrix_machine_sync_ipl() operates on the global g_machine —
     * valid since core->machine == &g_machine.
     */
    bellatrix_machine_sync_ipl();

    uint8_t ipl_after = (uint8_t)core->machine->current_ipl;
    if (ipl_after != ipl_before) {
        CORE2_LOG("INTREQ update IPL %u -> %u intreq=%04x intena=%04x",
                  (unsigned)ipl_before,
                  (unsigned)ipl_after,
                  (unsigned)core->machine->paula.irq.intreq,
                  (unsigned)core->machine->paula.irq.intena);
        XCORE_LOG("PAULA->CPU", "IPL=%u", (unsigned)ipl_after);
    }
}

bool core_audio_init(RuntimeCoreAudio *core, BellatrixMachine *machine)
{
    if (!core || !machine) {
        return false;
    }

    memset(core, 0, sizeof(*core));

    core->machine = machine;

    core->base.type = RUNTIME_CORE_AUDIO;
    core->base.name = "AUDIO";
    core->base.state = RUNTIME_CORE_RUNNING;

    core->last_master_cycles = 0;
    core->local_cycles = 0;

    core->enabled = true;

    CORE2_LOG("init");
    return true;
}

void core_audio_shutdown(RuntimeCoreAudio *core)
{
    if (!core) {
        return;
    }

    CORE2_LOG("shutdown cycles=%llu", (unsigned long long)core->local_cycles);
    core->enabled = false;
    core->base.state = RUNTIME_CORE_STOPPED;
}

void core_audio_reset(RuntimeCoreAudio *core)
{
    if (!core) {
        return;
    }

    CORE2_LOG("reset");
    core->last_master_cycles = 0;
    core->local_cycles = 0;
    core->enabled = true;
    core->base.state = RUNTIME_CORE_RUNNING;
}

void core_audio_step(RuntimeCoreAudio *core, uint64_t target_master_cycles)
{
    if (!core || !core->machine) {
        return;
    }

    if (!core->enabled || core->base.state != RUNTIME_CORE_RUNNING) {
        return;
    }

    if (target_master_cycles <= core->last_master_cycles) {
        return;
    }

    uint64_t delta = target_master_cycles - core->last_master_cycles;

    /*
     * Disk, serial, and interrupt consolidation get the full raw delta so no
     * Paula state falls behind regardless of step size.
     */
    uint32_t raw_cycles = (delta > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)delta;

    /*
     * Audio timeline is clamped to prevent a large catch-up burst after a
     * pause from corrupting the sample stream.
     */
    uint32_t audio_cycles = (uint32_t)(delta > 4096 ? 4096 : delta);

    core_audio_step_paula_audio(core, audio_cycles);
    core_audio_step_disk(core, raw_cycles);
    core_audio_step_serial(core, raw_cycles);
    core_audio_step_interrupts(core);

    core->last_master_cycles = target_master_cycles;
    core->local_cycles += audio_cycles;
}
