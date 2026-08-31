#include <config.h>

#include <aros/macros.h>

#include "rpihdmi-iec958.h"

/******************************************************************************
** MAI FIFO word format *******************************************************
******************************************************************************/

/*
 * The IEC 60958 framing is built by the hardware, not here.
 *
 * This file used to encode complete IEC958 subframes -- preamble in bits 3:0,
 * validity, user and channel-status bits at 30:28, even parity at 31 -- around
 * the sample. That is what a driver feeding a raw S/PDIF transmitter has to
 * do, and it is wrong for this block: with MAI_CONFIG programmed as it is in
 * rpihdmi-hwaccess.c, the MAI adds channel status, parity and preamble itself.
 * Every framing bit written here landed on top of a bit the hardware was also
 * writing.
 *
 * Bellatrix's earlier bare-metal driver (legacy branch,
 * src/host/raspi3/hdmi_audio.c), which produced sound on this exact hardware,
 * says so in as many words -- "HW does IEC958 framing; no software block
 * counting" -- and writes only the sample, with the low nibble cleared:
 *
 *     d = (uint32_t)(uint16_t)sample; d <<= 16; d >>= 4; d &= ~0xFu;
 *
 * which places the 16 sample bits at 27:12 and leaves 31:28 and 3:0 to the
 * block. That word format traces back to the Pi Zero W reference
 * (Sample_HDMI_DMA_Audio_03).
 *
 * Words reach the FIFO through DMA, so they are stored little-endian
 * regardless of the CPU's byte order.
 */
static inline ULONG mai_word(WORD sample)
{
    return AROS_LONG2LE((((ULONG) (UWORD) sample) << 12) & 0x0FFFF000UL);
}

void convert_mix_to_mai(WORD *src, ULONG *dst, ULONG frames)
{
    ULONG i;

    for (i = 0; i < frames; i++) {
        dst[i * 2]     = mai_word(src[i * 2]);
        dst[i * 2 + 1] = mai_word(src[i * 2 + 1]);
    }
}
