#ifndef BELLATRIX_CORE_MACHINE_H
#define BELLATRIX_CORE_MACHINE_H

#include <stdint.h>

#include "M68k.h"
#include "chipset/agnus/agnus.h"
#include "chipset/cia/cia.h"

typedef struct BellatrixMachine {
    uint64_t master_ticks;

    struct M68KState *cpu;

    AgnusState agnus;
    CIA_State  cia_a;
    CIA_State  cia_b;
} BellatrixMachine;

BellatrixMachine *bellatrix_machine_get(void);
void bellatrix_machine_init(struct M68KState *cpu);
void machine_step(BellatrixMachine *m, uint64_t ticks);

#endif