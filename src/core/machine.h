// src/core/machine.h

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cpu/cpu_backend.h"

#include "chipset/agnus/agnus.h"
#include "chipset/cia/cia.h"
#include "chipset/floppy/floppy_drive.h"
#include "chipset/denise/denise.h"
#include "chipset/paula/paula.h"
#include "chipset/rtc/rtc.h"
#include "input/controller_port.h"
#include "input/keyboard.h"

#include "bus/gayle/gayle.h"
#include "storage/iso/iso_image.h"

#include "debug/probe.h"
#include "debug/btrace.h"

#include "memory/memory.h"
#include "io/serial/uart_host.h"

typedef struct BellatrixDebug
{
    ProbeState  probe;
    BTraceState btrace;

    bool enable_probe;
    bool enable_btrace;

    bool dump_on_watchdog;
    bool dump_on_cpu_stop;
    bool dump_on_cpu_except;
    bool dump_on_ipl_change;

    uint32_t probe_last_n;
    uint32_t btrace_last_n;
    uint32_t copper_max_insn;

    bool     btrace_reads;
    bool     btrace_writes;
    bool     btrace_only_chipset;
    uint32_t btrace_addr_lo;
    uint32_t btrace_addr_hi;

} BellatrixDebug;

typedef struct BellatrixMachine
{
    CpuBackend *cpu_backend;

    /*
     * Memory is owned by the machine.
     * CPU, bus and chipset must use this object, not raw pointers.
     */
    BellatrixMemory memory;

    /*
     * Chipset components.
     */
    AgnusState agnus;
    Denise     denise;
    Paula      paula;
    CIA        cia_a;
    CIA        cia_b;
    RTCState   rtc;
    BellatrixKeyboard keyboard;
    BellatrixControllerPorts controller_ports;

    BellatrixDebug debug;

    uint64_t tick_count;
    uint8_t  current_ipl;

    /*
     * Fractional CPU → E-clock accumulator.
     */
    uint32_t cia_tick_acc;

    /*
     * DF0 drive state — signals CIA-A ext_pra.
     */
    FloppyDrive df0;

    /*
     * GAYLE IDE bridge — exposes an ATAPI CD-ROM device at $DA0000.
     * gayle.ide.cd.iso points to &iso below.
     */
    GayleState gayle;

    /*
     * ISO image attached to GAYLE — populated by bellatrix_machine_insert_iso
     * or bellatrix_machine_attach_iso_fn.  The gayle ATAPI uses a pointer to
     * this struct so updates here are immediately visible to GAYLE.
     */
    IsoImage iso;

    /* Scratch buffer for ATAPI data transfers (max ~2 sectors). */
    uint8_t gayle_atapi_buf[4096];

    /*
     * Host serial bridge used by the current machine-driven path.
     * This lets Emu68/Harness clock Paula serial without depending on the
     * separate runtime loop.
     */
    UARTHost uart_host;

} BellatrixMachine;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

BellatrixMachine *bellatrix_machine_get(void);

void bellatrix_machine_init(CpuBackend *cpu_backend);
void bellatrix_machine_reset(void);

void bellatrix_machine_attach_rom(const uint8_t *rom, uint32_t rom_size);

/* ------------------------------------------------------------------------- */
/* synchronization                                                           */
/* ------------------------------------------------------------------------- */

void bellatrix_machine_advance(uint32_t ticks);
void bellatrix_machine_sync_ipl(void);

/* ------------------------------------------------------------------------- */
/* bus protocol                                                              */
/* ------------------------------------------------------------------------- */

uint32_t bellatrix_machine_read(uint32_t addr, unsigned int size);
void     bellatrix_machine_write(uint32_t addr, uint32_t value, unsigned int size);

/* ------------------------------------------------------------------------- */
/* raw access to owned components                                            */
/* ------------------------------------------------------------------------- */

AgnusState *bellatrix_machine_agnus(void);
Denise     *bellatrix_machine_denise(void);
Paula      *bellatrix_machine_paula(void);
CIA        *bellatrix_machine_cia_a(void);
CIA        *bellatrix_machine_cia_b(void);
RTCState   *bellatrix_machine_rtc(void);

BellatrixMemory *bellatrix_machine_memory(void);

/* ------------------------------------------------------------------------- */
/* floppy media                                                              */
/* ------------------------------------------------------------------------- */

void bellatrix_machine_floppy_update(void);
int  bellatrix_machine_keyboard_rawkey(uint8_t rawkey, int pressed);
void bellatrix_machine_controller_set_device(unsigned port, unsigned device);
void bellatrix_machine_mouse_button(unsigned port, unsigned button, int pressed);
void bellatrix_machine_mouse_motion(unsigned port, int dx, int dy);
void bellatrix_machine_joystick_button(unsigned port, unsigned button, int pressed);
void bellatrix_machine_joystick_direction(unsigned port, unsigned direction, int pressed);

int  bellatrix_machine_insert_df0_adf(const uint8_t *adf, uint32_t adf_size);
void bellatrix_machine_eject_df0(void);

/* ------------------------------------------------------------------------- */
/* GAYLE / CD-ROM media                                                      */
/* ------------------------------------------------------------------------- */

/*
 * Attach an in-memory ISO image to the GAYLE CD-ROM device.
 * data must remain valid for the lifetime of the emulation session.
 * Returns 0 on success, -1 on invalid arguments.
 */
int bellatrix_machine_insert_iso(const void *data, size_t size);

/*
 * Attach a callback-based ISO image (bare-metal SD card, USB mass storage).
 * fn is called on every ATAPI READ(10) request; ctx and sector_count must
 * remain valid for the lifetime of the emulation session.
 * Returns 0 on success, -1 on invalid arguments.
 */
int bellatrix_machine_attach_iso_fn(iso_read_fn fn, void *ctx,
                                    uint32_t sector_count);

/* Remove the current ISO image from GAYLE. */
void bellatrix_machine_eject_iso(void);

/* ------------------------------------------------------------------------- */
/* debug access                                                              */
/* ------------------------------------------------------------------------- */

BellatrixDebug *bellatrix_machine_debug(void);

void bellatrix_machine_btrace_log(uint32_t addr,
                                  uint32_t value,
                                  unsigned int size,
                                  uint8_t dir,
                                  uint8_t impl);

void bellatrix_machine_btrace_set_filter(uint16_t filter);
