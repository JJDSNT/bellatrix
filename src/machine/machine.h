// src/machine/machine.h

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cpu/cpu_backend.h"

#include "machine/input/controller_port.h"
#include "machine/input/keyboard.h"

#include "storage/iso/iso_image.h"

#include "machine/bus/superbuster/superbuster.h"

#include "debug/probe.h"
#include "debug/btrace.h"

#include "machine/memory/memory.h"
#include "io/serial/uart_host.h"
#include "audio/mixer.h"

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

    uint32_t frame_counter;

    BellatrixKeyboard keyboard;
    BellatrixControllerPorts controller_ports;

    SuperBusterState superbuster;

    BellatrixDebug debug;

    uint64_t tick_count;
    uint8_t  current_ipl;

    /*
     * Host serial bridge used by the current machine-driven path.
     * This lets Emu68/Harness clock Paula serial without depending on the
     * separate runtime loop.
     */
    UARTHost uart_host;

    /* Mixed stereo samples drained from Rigel once per HBLANK (see
     * bellatrix_machine_on_audio_sample_ready()). No host output driver
     * consumes this yet — see AI_context/issue_paula_audio_timing_and_simd.md. */
    AudioMixerQueue audio_queue;

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
uint32_t bellatrix_machine_recommended_cpu_quantum(uint32_t max_cycles);

/* Hooks called by the chipset core (Core 1) after each Rigel step. */
void bellatrix_machine_on_frame_ready(void);
void bellatrix_machine_on_ipl_changed(uint8_t ipl);
void bellatrix_machine_on_audio_sample_ready(void);
/* Host-facing work that must accompany every actual Rigel advance in both
 * schedulers: 48 kHz PCM production and physical HDMI DMA refill. */
void bellatrix_machine_on_chipset_advanced(uint32_t cck_cycles);
void bellatrix_machine_post_chipset_step(void);

struct RigelContext *bellatrix_machine_rigel_ctx(void);

/* ------------------------------------------------------------------------- */
/* bus protocol                                                              */
/* ------------------------------------------------------------------------- */

uint32_t bellatrix_machine_read(uint32_t addr, unsigned int size);
void     bellatrix_machine_write(uint32_t addr, uint32_t value, unsigned int size);

/* ------------------------------------------------------------------------- */
/* raw access to owned components                                            */
/* ------------------------------------------------------------------------- */

BellatrixMemory *bellatrix_machine_memory(void);

/* ------------------------------------------------------------------------- */
/* floppy media                                                              */
/* ------------------------------------------------------------------------- */

int  bellatrix_machine_keyboard_rawkey(uint8_t rawkey, int pressed);
void bellatrix_machine_controller_set_device(unsigned port, unsigned device);
void bellatrix_machine_mouse_button(unsigned port, unsigned button, int pressed);
void bellatrix_machine_mouse_motion(unsigned port, int dx, int dy);
void bellatrix_machine_joystick_button(unsigned port, unsigned button, int pressed);
void bellatrix_machine_joystick_direction(unsigned port, unsigned direction, int pressed);

int  bellatrix_machine_insert_df_adf(unsigned drive, const uint8_t *adf,
                                     uint32_t adf_size);
int  bellatrix_machine_insert_df0_adf(const uint8_t *adf, uint32_t adf_size);
void bellatrix_machine_eject_df0(void);

/* ------------------------------------------------------------------------- */
/* CD-ROM media (via lide.device expansion)                                  */
/* ------------------------------------------------------------------------- */

int  bellatrix_machine_insert_iso(const void *data, size_t size);
int  bellatrix_machine_attach_iso_fn(iso_read_fn fn, void *ctx,
                                     uint32_t sector_count);
int  bellatrix_machine_attach_hdf_fn(int (*read_fn)(void *ctx, uint32_t lba,
                                                    uint32_t count,
                                                    uint8_t *buf),
                                     void *ctx, uint32_t sector_count);
void bellatrix_machine_eject_iso(void);

/* ------------------------------------------------------------------------- */
/* host-facing helpers                                                       */
/* ------------------------------------------------------------------------- */

const char *bellatrix_machine_backend_name(void);
void bellatrix_machine_serial_receive_byte(uint8_t byte);
int16_t bellatrix_machine_audio_left(void);
int16_t bellatrix_machine_audio_right(void);
int bellatrix_machine_serial_rx_pending(void);

void bellatrix_machine_rigel_trace_enable(bool enabled);

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
