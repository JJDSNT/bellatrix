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
#include "debug/probe.h"
#include "debug/btrace.h"
#include "memory/memory.h"

typedef struct BellatrixDebug
{
    /* --------------------------------------------------------------------- */
    /* always-on / low-cost collectors                                       */
    /* --------------------------------------------------------------------- */

    ProbeState  probe;
    BTraceState btrace;

    /* --------------------------------------------------------------------- */
    /* feature toggles                                                       */
    /* --------------------------------------------------------------------- */

    bool enable_probe;
    bool enable_btrace;

    bool dump_on_watchdog;
    bool dump_on_cpu_stop;
    bool dump_on_cpu_except;
    bool dump_on_ipl_change;

    /* --------------------------------------------------------------------- */
    /* dump policy                                                           */
    /* --------------------------------------------------------------------- */

    uint32_t probe_last_n;
    uint32_t btrace_last_n;
    uint32_t copper_max_insn;

    /* --------------------------------------------------------------------- */
    /* bus trace filters                                                     */
    /* --------------------------------------------------------------------- */

    bool     btrace_reads;
    bool     btrace_writes;
    bool     btrace_only_chipset;
    uint32_t btrace_addr_lo;
    uint32_t btrace_addr_hi;
} BellatrixDebug;

typedef struct BellatrixMachine
{
    CpuBackend *cpu_backend;

    BellatrixMemory memory;

    Agnus    agnus;
    Denise   denise;
    Paula    paula;
    CIA      cia_a;
    CIA      cia_b;
    RTCState rtc;

    BellatrixDebug debug;

    uint64_t tick_count;
    uint8_t  current_ipl;
    uint32_t cia_tick_acc;   /* fractional CPU→E-clock accumulator (÷10) */

    FloppyDrive df0;         /* DF0 drive state — signals CIA-A ext_pra */
} BellatrixMachine;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

BellatrixMachine *bellatrix_machine_get(void);

void bellatrix_machine_init(CpuBackend *cpu_backend);
void bellatrix_machine_reset(void);

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

Agnus    *bellatrix_machine_agnus(void);
Denise   *bellatrix_machine_denise(void);
Paula    *bellatrix_machine_paula(void);
CIA      *bellatrix_machine_cia_a(void);
CIA      *bellatrix_machine_cia_b(void);
RTCState *bellatrix_machine_rtc(void);

/* ------------------------------------------------------------------------- */
/* debug access                                                              */
/* ------------------------------------------------------------------------- */

BellatrixDebug *bellatrix_machine_debug(void);

/* floppy — call after any CIA-B PRB write so CIA-A ext_pra is updated */
void bellatrix_machine_floppy_update(void);

/* btrace wrappers — route calls through the machine's debug.btrace instance */
void bellatrix_machine_btrace_log(uint32_t addr, uint32_t value,
                                  unsigned int size, uint8_t dir, uint8_t impl);
void bellatrix_machine_btrace_set_filter(uint16_t filter);
