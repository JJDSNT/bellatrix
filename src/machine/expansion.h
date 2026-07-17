#ifndef BELLATRIX_MACHINE_EXPANSION_H
#define BELLATRIX_MACHINE_EXPANSION_H

/*
 * Bellatrix expansion registry (the pre-Emu68-convergence path).
 *
 * The board mechanism we are converging on is Emu68's: a minimal
 * `struct ExpansionBoard` that self-registers into a linker section and whose
 * map() installs a DIRECT region (see machine/bus/board_registry.h). This
 * registry — bellatrix_expansion_register() + bus_ops (owns_address/read/write)
 * over the Zorro II/III registries — predates that and is the older approach.
 *
 * It is kept alive for exactly one reason: **lide** (external/lide.device),
 * which provides ISO (CD-ROM) and HDF (hard-disk image) support and is the one
 * expansion Emu68 does not give us. lide is a *mixed* board — a ROM plus
 * side-effecting ATA/IDE registers served per-access through bus_ops — which
 * the Emu68 DIRECT-only board model does not express. Retiring this path
 * therefore waits on lide being re-expressed as an Emu68-style board (DIRECT
 * ROM) plus an EXTERNAL register window (routed by the Super Buster / bus
 * dispatch). Until then, do not delete this registry.
 */

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

typedef struct BellatrixExpansionBusOps {
    int      (*owns_address)(BellatrixExpansion *exp, uint32_t addr);
    uint32_t (*read)(BellatrixExpansion *exp, uint32_t addr, unsigned int size);
    void     (*write)(BellatrixExpansion *exp, uint32_t addr, uint32_t value,
                      unsigned int size);
} BellatrixExpansionBusOps;

typedef struct BellatrixExpansionDesc {
    const char *id;
    const char *name;
    BellatrixExpansionKind kind;
    uint32_t priority;
    void *userdata;
    const BellatrixZorro2BoardDesc *zorro2_board;
    const BellatrixExpansionBusOps *bus_ops;
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
int bellatrix_expansion_bus_read(
    BellatrixMachine *machine,
    uint32_t addr,
    unsigned int size,
    uint32_t *value
);
int bellatrix_expansion_bus_write(
    BellatrixMachine *machine,
    uint32_t addr,
    uint32_t value,
    unsigned int size
);

#ifdef __cplusplus
}
#endif

#endif
