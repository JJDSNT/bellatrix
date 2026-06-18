#ifndef BELLATRIX_HOST_RASPI3_CONSOLE_LOG_H
#define BELLATRIX_HOST_RASPI3_CONSOLE_LOG_H

/*
 * kprintf shares the mini-UART with Paula's emulated Amiga serial port —
 * PL011 belongs to Bluetooth, in every build. Paula's TX drain always has
 * priority (see machine_step_host_serial_rigel() in machine_rigel_step.c);
 * kprintf only fills the idle gaps via a non-blocking ring buffer. See
 * AI_context/issue_logging_miniuart.md.
 */

/* Called once, right after setup_serial() in emu68/src/aarch64/start.c,
 * before the first kprintf. */
void console_log_init(void);

/* Called once per quantum, immediately after Paula's TX FIFO is drained to
 * hardware (machine_step_host_serial_rigel() in machine_rigel_step.c) —
 * never before, so Paula's bytes always reach the wire first. */
void console_log_drain(void);

#endif
