#ifndef BELLATRIX_MACHINE_EXPANSION_H
#define BELLATRIX_MACHINE_EXPANSION_H

#include <stdint.h>
#include <stddef.h>

#include "machine/bus/zorro2/zorro2_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BellatrixMachine BellatrixMachine;
typedef struct BellatrixExpansion BellatrixExpansion;

typedef enum BellatrixExpansionKind {
    BELLATRIX_EXPANSION_BOARD = 1,
    BELLATRIX_EXPANSION_DEVICE,
    BELLATRIX_EXPANSION_SERVICE
} BellatrixExpansionKind;

typedef struct BellatrixExpansionOps {
    int  (*attach)(BellatrixExpansion *exp, BellatrixMachine *machine);
    void (*reset)(BellatrixExpansion *exp);
    void (*shutdown)(BellatrixExpansion *exp);
    void (*destroy)(BellatrixExpansion *exp);
} BellatrixExpansionOps;

typedef struct BellatrixExpansionDesc {
    const char *id;
    const char *name;
    BellatrixExpansionKind kind;
    uint32_t priority;
    void *userdata;
    const BellatrixZorro2BoardDesc *zorro2_board;
    const BellatrixExpansionOps *ops;
} BellatrixExpansionDesc;

struct BellatrixExpansion {
    BellatrixExpansionDesc desc;
    BellatrixMachine *machine;
    int attached;
};

int bellatrix_expansion_register(
    BellatrixMachine *machine,
    const BellatrixExpansionDesc *desc
);

BellatrixExpansion *bellatrix_expansion_find(
    BellatrixMachine *machine,
    const char *id
);

void bellatrix_expansion_reset_all(BellatrixMachine *machine);
void bellatrix_expansion_shutdown_all(BellatrixMachine *machine);

#ifdef __cplusplus
}
#endif

#endif
