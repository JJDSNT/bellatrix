// src/machine/expansions/rtg/rtg.h
//
// "bellatrix.rtg" — Zorro II P96-style framebuffer board.
// Register spec: docs/rtg_design.md. Guest driver: bellatrix.card (m68k),
// consumed by the AROS p96gfx HIDD.
//
// The board is host-backend agnostic: the harness blits the rendered
// frame to SDL/screenshots; the baremetal backend blits it to the VC4
// framebuffer. Rendering to RGBA is done here, once per fetched frame.

#ifndef BELLATRIX_EXPANSIONS_RTG_H
#define BELLATRIX_EXPANSIONS_RTG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct BellatrixMachine;

#define BELLATRIX_RTG_WINDOW      0x00400000u  /* 4MB Zorro II window   */
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

/* Register offsets (32-bit big-endian). Keep in sync with bellatrix.card. */
#define RTG_REG_ID            0x00u  /* RO 'BRTG'                        */
#define RTG_REG_VERSION       0x04u  /* RO spec version                  */
#define RTG_REG_VRAM_OFF      0x08u  /* RO VRAM offset in window         */
#define RTG_REG_VRAM_SIZE     0x0Cu  /* RO VRAM bytes                    */
#define RTG_REG_ENABLE        0x10u  /* 1 = RTG owns the video output    */
#define RTG_REG_MODE_W        0x14u
#define RTG_REG_MODE_H        0x18u
#define RTG_REG_FORMAT        0x1Cu  /* RGBFTYPE subset, see below       */
#define RTG_REG_BYTES_PER_ROW 0x20u
#define RTG_REG_PAN           0x24u  /* visible fb offset inside VRAM    */
#define RTG_REG_PAL_INDEX     0x28u
#define RTG_REG_PAL_DATA      0x2Cu  /* 0x00RRGGBB, autoincrements index */
#define RTG_REG_VBLANK        0x30u  /* RO frame counter                 */

#define RTG_ID_MAGIC   0x42525447u   /* 'BRTG' */
#define RTG_SPEC_VERSION 1u

/* RGBFTYPE values used (match p96gfx_rtg.h) */
#define RTG_FMT_CLUT      1u
#define RTG_FMT_A8R8G8B8  6u
#define RTG_FMT_R5G6B5    10u

typedef struct BellatrixRtgFrame {
    const uint8_t *pixels;  /* RGBA8888, tightly packed */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;         /* bytes per row (= width * 4) */
} BellatrixRtgFrame;

/* Register the board on the Zorro II bus. Returns 0 on success. */
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
