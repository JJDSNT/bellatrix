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

#endif
