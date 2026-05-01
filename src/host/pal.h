// src/host/pal.h
//
// Platform Abstraction Layer — public interface.
// The chipset code includes only this header; it never references
// platform-specific headers directly.

#ifndef _BELLATRIX_PAL_H
#define _BELLATRIX_PAL_H

#include <stdint.h>

typedef struct PAL_KeyEvent {
    uint32_t keycode;
    uint32_t scancode;
    uint32_t host_key;
    uint8_t pressed;
} PAL_KeyEvent;

enum {
    PAL_HOST_KEY_NONE = 0,
    PAL_HOST_KEY_0,
    PAL_HOST_KEY_1,
    PAL_HOST_KEY_2,
    PAL_HOST_KEY_3,
    PAL_HOST_KEY_4,
    PAL_HOST_KEY_5,
    PAL_HOST_KEY_6,
    PAL_HOST_KEY_7,
    PAL_HOST_KEY_8,
    PAL_HOST_KEY_9,
    PAL_HOST_KEY_SPACE,
    PAL_HOST_KEY_RETURN,
    PAL_HOST_KEY_KP_ENTER,
    PAL_HOST_KEY_ESCAPE,
    PAL_HOST_KEY_UP,
    PAL_HOST_KEY_DOWN,
    PAL_HOST_KEY_LEFT,
    PAL_HOST_KEY_RIGHT,
    PAL_HOST_KEY_KP_0,
    PAL_HOST_KEY_KP_1,
    PAL_HOST_KEY_KP_2,
    PAL_HOST_KEY_KP_3,
    PAL_HOST_KEY_KP_4,
    PAL_HOST_KEY_KP_5,
    PAL_HOST_KEY_KP_6,
    PAL_HOST_KEY_KP_7,
    PAL_HOST_KEY_KP_8,
    PAL_HOST_KEY_KP_9,
};

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

/* ---- Runtime lifecycle ---- */

void PAL_Runtime_Init(void);
void PAL_Runtime_Shutdown(void);
void PAL_Runtime_Poll(void);
void PAL_Runtime_ReportCpuProgress(uint32_t cycles);
uint32_t PAL_Runtime_GetPendingIPL(void);
void PAL_Runtime_WakeupChipset(void);
void PAL_Runtime_MmioBarrier(void);

uint64_t PAL_Time_ReadCounter(void);
uint64_t PAL_Time_GetFrequency(void);

/* ---- Dedicated ARM core ---- */

/* Launch chipset loop on a secondary ARM core via spin-table.
   Stub in Phase 0. */
void PAL_Core_LaunchChipset(void (*entry)(void));
void PAL_Core_SetMulticoreEnabled(int enabled);
int  PAL_Core_IsMulticoreEnabled(void);
void PAL_Core_Sync(void);

/* ---- Host display event helpers (harness / posix only) ---- */

/* Returns 1 to keep running, 0 if the user closed the window or pressed Esc.
 * No-op stub on bare-metal targets. */
int  pal_sdl_poll_events(void);
int  pal_sdl_mouse_right_down(void);
int  pal_sdl_any_key_down(void);
int  pal_sdl_pop_key_event(PAL_KeyEvent *event);

/* Update window title (e.g. show FPS). No-op on bare-metal. */
void pal_sdl_set_title(const char *title);

/* ---- Harness serial helpers (posix only) ---- */

/* Configure serial presentation for the host harness from env var
 * HARNESS_SERIAL_MODE=line|raw|ansi. No-op on bare-metal. */
void PAL_HarnessSerial_ConfigureFromEnv(void);

/* Return current harness serial mode name. Bare-metal returns "line". */
const char *PAL_HarnessSerial_ModeName(void);

/* Emit one UART TX byte according to the configured harness serial mode. */
void PAL_HarnessSerial_WriteByte(uint8_t byte);
int  PAL_HarnessSerial_ReadByte(uint8_t *byte);

#endif /* _BELLATRIX_PAL_H */
