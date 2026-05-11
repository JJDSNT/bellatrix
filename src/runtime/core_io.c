#include "runtime/core_io.h"

#include <string.h>

#include "chipset/cia/cia.h"
#include "input/keyboard.h"
#include "io/serial/uart_host.h"

/*
 * Core 3 — Physical peripherals domain.
 *
 * Owns: CIA-A/CIA-B timers, TOD, keyboard protocol, physical host UART.
 *
 * Does NOT own: Paula registers, INTREQ/INTENA/IPL, disk DMA, audio DMA.
 * Those live in Core 2 (core_audio / Paula domain).
 *
 * Floppy drive physical state is updated via CIA-B PRB writes in the bus
 * dispatch path (machine_floppy_update) — no periodic step needed here.
 */

static void core_io_step_cia(RuntimeCoreIO *core, uint32_t cia_ticks)
{
    if (!core || !core->machine) {
        return;
    }

    cia_step(&core->machine->cia_a, cia_ticks);
    cia_step(&core->machine->cia_b, cia_ticks);
}

static void core_io_step_host_serial(RuntimeCoreIO *core)
{
    if (!core || !core->machine) {
        return;
    }

    /*
     * Drain Paula TX → host UART and inject host RX → Paula serial RX.
     * If no backend is open (uart_host.enabled == false), poll is a no-op.
     */
    uart_host_poll(&core->machine->uart_host);
}

bool core_io_init(RuntimeCoreIO *core, BellatrixMachine *machine)
{
    if (!core || !machine) {
        return false;
    }

    memset(core, 0, sizeof(*core));

    core->machine = machine;
    core->running = true;
    core->local_cycles = 0;

    return true;
}

void core_io_shutdown(RuntimeCoreIO *core)
{
    if (!core) {
        return;
    }

    core->running = false;
}

bool core_io_open_debug_serial(RuntimeCoreIO *core)
{
    (void)core;
    /* On bare metal, serial is opened by bellatrix_init().
     * This stub is kept for harness compatibility. */
    return false;
}

void core_io_step(RuntimeCoreIO *core, uint32_t cycles)
{
    if (!core || !core->running || !core->machine) {
        return;
    }

    core->local_cycles += cycles;

    BellatrixMachine *m = core->machine;

    /*
     * CIA runs at E-clock = CPU / 10.
     * Use the machine's shared accumulator to avoid drift on small quanta.
     */
    m->cia_tick_acc += cycles;
    uint32_t cia_ticks = m->cia_tick_acc / 10u;
    m->cia_tick_acc   %= 10u;

    if (cia_ticks > 0) {
        core_io_step_cia(core, cia_ticks);
    }

    /*
     * CIA-B TOD is clocked from Agnus hsync pulses.
     * Agnus accumulates them in agnus.hsync_pulses; we drain here.
     */
    if (m->agnus.hsync_pulses > 0) {
        cia_tod_pulse(&m->cia_b, m->agnus.hsync_pulses);
        m->agnus.hsync_pulses = 0;
    }

    /* Keyboard shift register / handshake (CIA-A SP + CNT lines). */
    bellatrix_keyboard_step(&m->keyboard, &m->cia_a);

    /* Physical serial host backend: TX drain → host, RX inject → Paula. */
    core_io_step_host_serial(core);
}
