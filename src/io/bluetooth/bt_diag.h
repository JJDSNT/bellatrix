#ifndef BELLATRIX_IO_BT_DIAG_H
#define BELLATRIX_IO_BT_DIAG_H

/* RAM text ring for Bluetooth bring-up diagnostics.
 *
 * Bootstrap breadcrumbs remain in RAM for BTSCAN.TXT in addition to being
 * mirrored to the independent AUX miniUART log. PL011 always belongs to BT.
 */

#include <stdint.h>

/* printf-style append; also mirrors to kprintf when the console is alive. */
void bt_diag_log(const char *fmt, ...);

/* Read access for the SD dump: contiguous snapshot into out (NUL-terminated),
 * returns number of bytes written (excluding the NUL). */
uint32_t bt_diag_snapshot(char *out, uint32_t cap);

#endif /* BELLATRIX_IO_BT_DIAG_H */
