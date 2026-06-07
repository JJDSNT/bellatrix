// src/runtime/core_io.c
//
// Core 3 — Physical peripherals domain.
//
// Owns: USB host stack, Bluetooth host stack.
// Serial host (uart_host) is polled via bellatrix_machine_post_chipset_step()
// on Core 1 to keep it co-located with Rigel's serial state.

#include "runtime/core_io.h"
#include "runtime/runtime.h"

#include <string.h>

#include "io/usb/usb_host.h"
#include "debug/core_log.h"

bool core_io_init(RuntimeCoreIO *core, BellatrixMachine *machine)
{
    if (!core || !machine) {
        return false;
    }

    memset(core, 0, sizeof(*core));

    core->machine = machine;
    core->running = true;
    core->local_cycles = 0;
    usb_host_init(&core->usb_host);

    CORE3_LOG("init");
    return true;
}

void core_io_shutdown(RuntimeCoreIO *core)
{
    if (!core) {
        return;
    }

    extern BellatrixRuntime g_runtime;
    bt_host_shutdown(&g_runtime.bluetooth);
    usb_host_shutdown(&core->usb_host);

    CORE3_LOG("shutdown cycles=%llu", (unsigned long long)core->local_cycles);
    core->running = false;
}

bool core_io_open_debug_serial(RuntimeCoreIO *core)
{
    (void)core;
    return false;
}

/* Called by Core 3 via bellatrix_runtime_io_step() in pal_core.c. */
void core_io_step(RuntimeCoreIO *core, uint32_t cycles)
{
    if (!core || !core->running) {
        return;
    }

    core->local_cycles += cycles;

    extern BellatrixRuntime g_runtime;
    bt_host_step(&g_runtime.bluetooth);
    usb_host_step(&core->usb_host);
}

/* Weak override — called from Core 3's loop in pal_core.c. */
__attribute__((weak)) void bellatrix_runtime_io_step(uint64_t now, uint64_t freq)
{
    (void)now;
    (void)freq;

    extern BellatrixRuntime g_runtime;
    core_io_step(&g_runtime.io, 1u);
}
