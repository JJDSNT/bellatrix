#ifndef BELLATRIX_BUS_BOARD_REGISTRY_H
#define BELLATRIX_BUS_BOARD_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Emu68's expansion-board descriptor is the shared shape across backends. The
 * bare-metal Emu68 build already defines and walks these (emu68/include/boards.h
 * + the emu68/src/boards sources, driven by vectors.c). We reuse that exact
 * struct here so a board authored once serves both the Emu68 backend (its native
 * linker-section walker) and the Musashi/harness backend (the walker below),
 * with no mirror type to keep in sync.
 */
#include <boards.h>   /* struct ExpansionBoard */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Self-registration: a board drops its descriptor pointer into a linker section
 * and is discovered by walking that section — no central register_board() call.
 * Adding a board is adding a .c to the build; removing it is deleting the file.
 *
 * A C-identifier section name (no dot) makes the linker synthesise
 * __start_/__stop_ boundary symbols on any ELF target, including the POSIX
 * harness, with no linker script. (The bare-metal Emu68 backend can instead
 * place boards into Emu68's own `.boards.z2`/`.boards.z3` sections and let its
 * native vectors.c walker consume them; that is a per-build placement choice.)
 *
 * IMPORTANT: boards must be linked as direct objects, never inside a static
 * archive (.a) — an archive member with no otherwise-referenced symbol is
 * dropped and its registration silently lost, taking the boundary symbols with
 * it. Harness (add_executable source list) and product (BELLATRIX_SOURCES target
 * sources) both link boards directly, so this holds as long as boards never move
 * into a library.
 */
#ifndef BELLATRIX_BOARDS_SECTION
#define BELLATRIX_BOARDS_SECTION "bellatrix_boards"
#endif

/*
 * Z2/Z3 are separate macros so intent is explicit at the call site (and so the
 * section can later be split per bus for the Emu68 native path); today both land
 * in the same table and are told apart by the descriptor's is_z3 field.
 */
#define BELLATRIX_REGISTER_BOARD_Z2(sym)                                     \
    static struct ExpansionBoard *const _bxboard_##sym                       \
        __attribute__((used, section(BELLATRIX_BOARDS_SECTION))) = &(sym)
#define BELLATRIX_REGISTER_BOARD_Z3(sym)                                     \
    static struct ExpansionBoard *const _bxboard_##sym                       \
        __attribute__((used, section(BELLATRIX_BOARDS_SECTION))) = &(sym)

/* Iteration over the discovered table, in link order. */
size_t                 bellatrix_board_count(void);
struct ExpansionBoard *bellatrix_board_at(size_t index);

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
