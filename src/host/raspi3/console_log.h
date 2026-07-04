#ifndef BELLATRIX_HOST_RASPI3_CONSOLE_LOG_H
#define BELLATRIX_HOST_RASPI3_CONSOLE_LOG_H

#include <stdint.h>

/*
 * kprintf owns the mini-UART from Emu68 setup_serial() onward; PL011 belongs
 * to Bluetooth in every build. Before Paula's host serial bridge opens,
 * kprintf writes directly to mini-UART. Once Paula shares the wire, Paula's
 * TX drain has priority and kprintf falls back to an opportunistic ring.
 */

/* Called by Emu68's setup_serial() for Bellatrix before the first kprintf. */
void bellatrix_console_log_init_early(uint32_t core_hz);
void bellatrix_console_log_reclock(uint32_t core_hz);

/* Switch kprintf from direct mini-UART writes to the opportunistic ring used
 * when Paula shares the physical mini-UART. Leave direct mode enabled when
 * no Paula hardware bridge owns this UART. */
void console_log_set_deferred(void);

/* Called once per quantum, immediately after Paula's TX FIFO is drained to
 * hardware (machine_step_host_serial_rigel() in machine_rigel_step.c) —
 * never before, so Paula's bytes always reach the wire first. */
void console_log_drain(void);

#endif
