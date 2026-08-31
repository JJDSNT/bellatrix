#ifndef BELLATRIX_AMIGA_FRAME_H
#define BELLATRIX_AMIGA_FRAME_H

#include "rigel/rigel.h"

/*
 * The Denise frame aperture: where the guest can read what the chipset drew.
 *
 * Two regions, installed above the classic 24-bit domain because that is where
 * the machine stops being an Amiga and starts being ours:
 *
 *   $01000000  the frame itself, DIRECT, backed by a buffer Bellatrix owns
 *   $01200000  a descriptor page, EXTERNAL, saying where and how big it is
 *
 * The guest never sees Rigel's own frame buffer. That one lives in Emu68's
 * heap, outside the guest range on purpose (patch emu68/0007), and its
 * lifetime ends at the next rigel_step. One copy per frame buys a stable
 * buffer with a stable address, which is what any consumer needs -- an AROS
 * display driver reading it, or later an HVS plane scanning it.
 */
#define AMIGA_FRAME_BASE        0x01000000UL
#define AMIGA_FRAME_SIZE        0x00200000UL
#define AMIGA_FRAME_DESC_BASE   0x01200000UL
#define AMIGA_FRAME_DESC_SIZE   0x00001000UL

/* Descriptor register offsets, byte offsets from AMIGA_FRAME_DESC_BASE. */
#define AMIGA_FRAME_REG_MAGIC   0x00u   /* 'DNSE' once the aperture exists   */
#define AMIGA_FRAME_REG_VERSION 0x04u
#define AMIGA_FRAME_REG_BASE    0x08u   /* guest address of the frame        */
#define AMIGA_FRAME_REG_PITCH   0x0cu   /* bytes between row starts          */
#define AMIGA_FRAME_REG_WIDTH   0x10u   /* visible pixels per row            */
#define AMIGA_FRAME_REG_HEIGHT  0x14u   /* visible rows                      */
#define AMIGA_FRAME_REG_FLAGS   0x18u   /* bit 0: a frame has been published */
#define AMIGA_FRAME_REG_COUNT   0x1cu   /* frames published, low 32 bits     */
#define AMIGA_FRAME_REG_PHYS    0x20u   /* ARM physical address of the frame */
/*
 * What the chipset was doing while it composed this frame -- Rigel's own
 * rigel_frame_flags_t, verbatim.
 *
 * A consumer needs this to tell a picture from an idle machine. "A frame has
 * been published" (REG_FLAGS bit 0) is true from the first VBLANK onwards and
 * stays true forever, so it cannot answer the only question a display asks:
 * is there anything to show? COPPER_ACTIVE can, because the Copper executes a
 * MOVE only when something programmed a display.
 */
#define AMIGA_FRAME_REG_CHIPFLAGS 0x24u

/* Mirrors rigel_frame_flags_t; kept here so a guest needs no Rigel header. */
#define AMIGA_FRAME_CHIP_HAM            0x01u
#define AMIGA_FRAME_CHIP_DUAL_PLAYFIELD 0x02u
#define AMIGA_FRAME_CHIP_SPRITES_ACTIVE 0x04u
#define AMIGA_FRAME_CHIP_COPPER_ACTIVE  0x08u

#define AMIGA_FRAME_MAGIC       0x444e5345UL   /* 'DNSE' */
#define AMIGA_FRAME_VERSION     3UL
#define AMIGA_FRAME_FLAG_VALID  0x00000001UL

/* Install the two regions. Called once, after the static machine map. */
void amiga_frame_init(void);

/* Copy one finished frame into the aperture and update the descriptor. */
void amiga_frame_publish(RigelContext *ctx);

#endif /* BELLATRIX_AMIGA_FRAME_H */
