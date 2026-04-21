#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "M68k.h"

#include "chipset/agnus/agnus.h"
#include "chipset/cia/cia.h"
#include "chipset/denise/denise.h"
#include "chipset/paula/paula.h"
#include "chipset/rtc/rtc.h"
#include "memory/memory.h"

typedef struct BellatrixMachine
{
    struct M68KState *cpu;

    BellatrixMemory memory;

    Agnus    agnus;
    Denise   denise;
    Paula    paula;
    CIA      cia_a;
    CIA      cia_b;
    RTCState rtc;

    uint64_t tick_count;
    uint8_t  current_ipl;
} BellatrixMachine;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

BellatrixMachine *bellatrix_machine_get(void);

void bellatrix_machine_init(struct M68KState *cpu);
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
/* raw access to owned components                                             */
/* ------------------------------------------------------------------------- */

Agnus    *bellatrix_machine_agnus(void);
Denise   *bellatrix_machine_denise(void);
Paula    *bellatrix_machine_paula(void);
CIA      *bellatrix_machine_cia_a(void);
CIA      *bellatrix_machine_cia_b(void);
RTCState *bellatrix_machine_rtc(void);