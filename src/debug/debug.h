#ifndef BELLATRIX_DEBUG_DEBUG_H
#define BELLATRIX_DEBUG_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct BellatrixMachine;

void bellatrix_debug_dump(struct BellatrixMachine *m);
void bellatrix_debug_dump_probe(struct BellatrixMachine *m, uint32_t last_n);
void bellatrix_debug_dump_all(struct BellatrixMachine *m,
                              uint32_t probe_last_n,
                              uint32_t copper_max_insn);

#ifdef __cplusplus
}
#endif

#endif