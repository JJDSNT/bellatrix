#ifndef BELLATRIX_HOST_RASPI3_VC_MAILBOX_H
#define BELLATRIX_HOST_RASPI3_VC_MAILBOX_H

#include <stdint.h>
#include <stdbool.h>

/* VideoCore mailbox property-channel primitives. Always compiled (no
 * BTSTACK dependency) so early boot code can query the real core clock
 * before any other subsystem exists. */

void vc_mbox_send(uint32_t channel, uint32_t data);
bool vc_mbox_recv(uint32_t channel, uint32_t *out);

/* Real VPU/core clock — UART baud divisors track it, so never assume a
 * fixed value (garbled console when the firmware runs the core at a
 * different rate than expected). Returns 0 on mailbox failure; callers
 * should fall back to a safe default (e.g. 250000000u). */
uint32_t vc_get_core_clock_hz(void);

/* HDMI pixel (TMDS) clock in Hz, for computing the audio CTS. Returns 0 on
 * mailbox failure; callers fall back to the configured mode's clock. */
uint32_t vc_get_pixel_clock_hz(void);

/* Current ARM core clock in Hz. The firmware silently caps this to 600 MHz
 * under undervoltage/thermal throttling, so performance numbers are
 * meaningless without it. Returns 0 on mailbox failure. */
uint32_t vc_get_arm_clock_hz(void);

/* GET_THROTTLED flags: bit0 undervoltage now, bit1 ARM freq capped now,
 * bit2 throttled now; bits 16-18 the same conditions since boot (sticky).
 * Returns 0xFFFFFFFF on mailbox failure. */
uint32_t vc_get_throttled(void);

#endif
