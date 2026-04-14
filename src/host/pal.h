// src/host/pal.h
//
// Platform Abstraction Layer — public interface.
// The chipset code includes only this header; it never references
// platform-specific headers directly.

#ifndef _BELLATRIX_PAL_H
#define _BELLATRIX_PAL_H

#include <stdint.h>

/* ---- Debug / Serial ---- */

/* Initialize UART for debug output. baud is informational on platforms
   where Emu68 has already configured the UART (raspi3). */
void PAL_Debug_Init(uint32_t baud);

void PAL_Debug_PutC(char c);
void PAL_Debug_Print(const char *str);
void PAL_Debug_PrintHex(uint32_t val);

/* ---- IPL injection into the JIT loop ---- */

/* Set the pending M68K interrupt priority level (1–7) and notify the JIT.
   Must be called with a DMB barrier so the JIT sees the update. */
void PAL_IPL_Set(uint8_t ipl_level);

/* Clear IPL after interrupt acknowledgement. */
void PAL_IPL_Clear(void);

/* ---- Chipset timer (50 Hz PAL frame clock) ---- */

/* Configure the chipset timer. cb is called at hz per second on the
   dedicated ARM core. Stub in Phase 0. */
void PAL_ChipsetTimer_Init(uint32_t hz, void (*cb)(void));
void PAL_ChipsetTimer_Start(void);
void PAL_ChipsetTimer_Stop(void);

/* ---- Framebuffer ---- */

/* Allocate framebuffer via VC4 mailbox. Returns 0 on success.
   Stub in Phase 0. */
int      PAL_Video_Init(uint32_t w, uint32_t h, uint32_t bpp);
uint32_t *PAL_Video_GetBuffer(void);
void     PAL_Video_Flip(void);
void     PAL_Video_SetPalette(uint8_t idx, uint32_t rgb);

/* ---- Dedicated ARM core ---- */

/* Launch chipset loop on a secondary ARM core via spin-table.
   Stub in Phase 0. */
void PAL_Core_LaunchChipset(void (*entry)(void));
void PAL_Core_Sync(void);

#endif /* _BELLATRIX_PAL_H */
