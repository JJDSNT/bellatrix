#ifndef BELLATRIX_BUS_BOARD_REGISTRY_H
#define BELLATRIX_BUS_BOARD_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Self-registering expansion boards, modelled on Emu68's linker-section board
 * table (emu68/include/boards.h + the emu68/src/boards sources). A board drops a
 * descriptor pointer into a linker section; the set is discovered by walking
 * that section — there is no central register_board() call. Adding a board is
 * adding a .c to the build; removing it is deleting the file.
 *
 * BellatrixBoard is laid out to match Emu68's `struct ExpansionBoard` field for
 * field, so the same descriptor can be dropped into Emu68's own
 * `.boards.z2`/`.boards.z3` sections and walked by its native vectors.c
 * autoconfig handler in the bare-metal build. In the POSIX harness we use a
 * C-identifier section name so the linker synthesises `__start_`/`__stop_`
 * boundary symbols with no linker script — see bellatrix_board_count().
 *
 * IMPORTANT: boards must be linked as direct objects, never inside a static
 * archive (.a). An archive member with no otherwise-referenced symbol is
 * dropped by the linker and its self-registration silently lost (and the
 * boundary symbols may vanish with it). The harness (add_executable source
 * list) and the product (BELLATRIX_SOURCES target sources) both link boards
 * directly, so this holds as long as boards never move into a library.
 */

typedef struct BellatrixBoard {
    const void *rom_file;   /* Autoconfig image (nibble-encoded); may be a ROM  */
    uint32_t    rom_size;   /* region/backing size                              */
    uint32_t    map_base;   /* filled at autoconfig from the guest-assigned base */
    uint32_t    is_z3;      /* 0 = Zorro II, 1 = Zorro III                       */
    uint32_t    enabled;    /* participates in autoconfig                        */
    void      (*map)(struct BellatrixBoard *board); /* install region (backend) */
} BellatrixBoard;

/*
 * Section the descriptor pointers land in. A single C-identifier name (no dot)
 * lets the linker provide __start_/__stop_ automatically on any ELF target,
 * including the POSIX harness. The bare-metal Emu68 backend can instead reuse
 * Emu68's own dotted sections and native walker; flip this define (and use the
 * __boards_start iterator) when wiring that path.
 */
#ifndef BELLATRIX_BOARDS_SECTION
#define BELLATRIX_BOARDS_SECTION "bellatrix_boards"
#endif

/*
 * Drop a board into the table. Z2/Z3 are separate macros so intent is explicit
 * at the call site (and so the section can later be split per bus for the Emu68
 * native path); today both land in the same table and are told apart by is_z3.
 */
#define BELLATRIX_REGISTER_BOARD_Z2(sym)                                   \
    static BellatrixBoard *const _bxboard_##sym                            \
        __attribute__((used, section(BELLATRIX_BOARDS_SECTION))) = &(sym)
#define BELLATRIX_REGISTER_BOARD_Z3(sym)                                   \
    static BellatrixBoard *const _bxboard_##sym                            \
        __attribute__((used, section(BELLATRIX_BOARDS_SECTION))) = &(sym)

/* Iteration over the discovered table, in link order. */
size_t          bellatrix_board_count(void);
BellatrixBoard *bellatrix_board_at(size_t index);

/*
 * Autoconfig driver over the discovered table, mirroring Emu68's vectors.c:
 * config reads answer straight from the board's rom_file; the guest-written
 * base at offset 0x44 (Z3) / 0x48 (Z2) completes assignment and calls map();
 * a shutup (0x4C/0x4E) skips to the next board. reset() rewinds to the first.
 */
void    bellatrix_boards_autoconfig_reset(void);
uint8_t bellatrix_boards_autoconfig_read8(uint32_t addr);
void    bellatrix_boards_autoconfig_write(uint32_t addr, uint32_t value,
                                          unsigned int size);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_BUS_BOARD_REGISTRY_H */
