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
 * expansion Emu68 does not give us. lide is a *fully EXTERNAL* board: its ROM
 * is address-transformed (nibble-encoded bootldr, BYTEWIDE stride-2 device
 * binary, odd-address -> 0xFF) and it carries side-effecting ATA/IDE registers,
 * so it cannot be a plain DIRECT region and must be served per access.
 *
 * As of the board_registry-live work, lide's *Autoconfig* is presented by the
 * Emu68-style board_registry (the single Autoconfig authority): lide drops a
 * `struct ExpansionBoard` with map == NULL, so the walker only latches the
 * guest-assigned base into it (see lide_cdrom.c). This registry now serves only
 * the board's *window* — its per-access read/write over the ATA registers and
 * transformed ROM (bus_ops), routed by machine_rigel_bus.c's is_z2_board_addr
 * via bellatrix_boards_external_window_owner(). Retiring this registry waits on
 * a shared EXTERNAL-window serving mechanism in board_registry itself; until
 * then, do not delete it. lide no longer registers in the legacy zorro2
 * Autoconfig state machine (that path is now RTG-only).
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
