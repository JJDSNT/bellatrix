#ifndef BELLATRIX_DEBUG_EMU_DEBUG_H
#define BELLATRIX_DEBUG_EMU_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct BellatrixMachine;

void emu_debug_dma(struct BellatrixMachine *m);
void emu_debug_copper(struct BellatrixMachine *m, uint32_t max_insn);
void emu_debug_mem(struct BellatrixMachine *m, uint32_t addr, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif