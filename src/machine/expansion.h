#ifndef BELLATRIX_MACHINE_EXPANSION_H
#define BELLATRIX_MACHINE_EXPANSION_H

/*
 * Bellatrix EXTERNAL-window serving registry.
 *
 * The board mechanism we converge on is Emu68's: a minimal `struct
 * ExpansionBoard` that self-registers into a linker section (see
 * machine/bus/board_registry.h). A DIRECT board's map() installs a region; an
 * EXTERNAL board (map == NULL) has the walker only latch its guest-assigned
 * base. board_registry is the single Autoconfig authority for both.
 *
 * board_registry's ExpansionBoard has no per-access read/write callbacks, so an
 * EXTERNAL board's *window* is served here: bellatrix_expansion_register() +
 * bus_ops (owns_address/read/write), routed from machine_rigel_bus.c's
 * is_z2/is_z3_board_addr (bellatrix_boards_external_window_owner) to the owning
 * board's bus_ops. Two boards use this today:
 *   - lide (external/lide.device): ISO + HDF, the one expansion Emu68 does not
 *     give us. Fully EXTERNAL — its ROM is address-transformed (nibble bootldr,
 *     BYTEWIDE device binary, odd address -> 0xFF) plus side-effecting ATA
 *     registers, so no DIRECT region can express it.
 *   - RTG (harness/lab-only): a Zorro III register/VRAM window.
 *
 * Retiring this registry waits on a shared EXTERNAL-window serving mechanism in
 * board_registry itself; until then, do not delete it. Neither board uses the
 * legacy zorro2/zorro3 Autoconfig registries any longer.
 */

#include <stdint.h>
#include <stddef.h>

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
