// src/machine/expansions/rtg/rtg.h
//
// "bellatrix.rtg" — Zorro III P96-style linear framebuffer board.
// Register spec: docs/rtg_design.md. Guest driver: bellatrix.card (m68k),
// consumed by the AROS p96gfx HIDD.
//
// The board is host-backend agnostic: it exposes a common scanout state for
// harness SDL/screenshots and a future Raspberry presenter. Rendering to RGBA
// is done here, once per fetched frame.

#ifndef BELLATRIX_EXPANSIONS_RTG_H
#define BELLATRIX_EXPANSIONS_RTG_H

#include <stdint.h>
#include "machine/expansions/rtg/rtg_scanout.h"

#ifdef __cplusplus
extern "C" {
#endif

struct BellatrixMachine;

#define BELLATRIX_RTG_WINDOW      0x00800000u  /* 8MB Zorro III window  */
#define BELLATRIX_RTG_REG_SIZE    0x00000100u  /* register file (256B)  */
#define BELLATRIX_RTG_ROM_OFF     0x00000100u  /* DiagArea+card ROM     */
#define BELLATRIX_RTG_ROM_SIZE    0x00002F00u  /* 11.75KB ROM budget    */
/* Card hunk offset within the window — must match CARDOFFSET in
 * cards/bellatrix.card/bootrom/cardldr.S (not shared: that file is
 * assembled standalone, see its own header comment). */
#define BELLATRIX_RTG_CARD_OFF    0x00002000u
#define BELLATRIX_RTG_VRAM_OFF    0x00003000u  /* VRAM offset in window */
#define BELLATRIX_RTG_VRAM_SIZE   (BELLATRIX_RTG_WINDOW - BELLATRIX_RTG_VRAM_OFF)

#define BELLATRIX_RTG_MANUFACTURER 0x07DBu
#define BELLATRIX_RTG_PRODUCT      0x10u

/* Register the board on the Zorro III bus. Returns 0 on success. */
int bellatrix_rtg_register(struct BellatrixMachine *m);

/* 1 when the guest set ENABLE and programmed a plausible mode. */
int bellatrix_rtg_active(void);

/* Render the current VRAM contents to RGBA and expose the frame.
 * Returns 1 on success (RTG active and mode valid). */
int bellatrix_rtg_get_frame(BellatrixRtgFrame *out);

/* Bump the VBLANK counter — call once per host video frame. */
void bellatrix_rtg_frame_tick(void);

/* Direct VRAM backing for CPU fast paths (returns NULL when the board
 * is not configured yet). base = m68k address of first VRAM byte. */
uint8_t *bellatrix_rtg_vram_ptr(uint32_t *base, uint32_t *size);

#ifdef __cplusplus
}
#endif

#endif /* BELLATRIX_EXPANSIONS_RTG_H */
