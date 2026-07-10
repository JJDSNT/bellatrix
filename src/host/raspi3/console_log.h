#ifndef BELLATRIX_HOST_RASPI3_CONSOLE_LOG_H
#define BELLATRIX_HOST_RASPI3_CONSOLE_LOG_H

#include <stdint.h>

/*
 * Core 0 owns the mini-UART during early boot; Core 3 owns it after runtime
 * launch. PL011 belongs to Bluetooth in every build. Runtime kprintf producers
 * enqueue without touching the UART; Core 3 gives Paula TX priority and drains
 * console output opportunistically.
 */

/* Called by Emu68's setup_serial() for Bellatrix before the first kprintf. */
void bellatrix_console_log_init_early(uint32_t core_hz);
void bellatrix_console_log_reclock(uint32_t core_hz);

/* Switch kprintf from direct mini-UART writes to the opportunistic ring used
 * when Paula shares the physical mini-UART. Leave direct mode enabled when
 * no Paula hardware bridge owns this UART. */
void console_log_set_deferred(void);

/* Called by Core 3 after the Paula TX queue has drained, or by the local
 * fallback in single-core builds. */
void console_log_drain(void);

#endif
