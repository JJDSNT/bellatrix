#ifndef BELLATRIX_IO_BT_DIAG_H
#define BELLATRIX_IO_BT_DIAG_H

/* RAM text ring for Bluetooth bring-up diagnostics.
 *
 * The PL011 console is muxed away (and kprintf disabled) while BTStack owns
 * the UART, so bootstrap breadcrumbs must survive in RAM until the launcher
 * can flush them to BTSCAN.TXT on the SD card.  Entries are plain text lines.
 */

#include <stdint.h>

/* printf-style append; also mirrors to kprintf when the console is alive. */
void bt_diag_log(const char *fmt, ...);

/* Read access for the SD dump: contiguous snapshot into out (NUL-terminated),
 * returns number of bytes written (excluding the NUL). */
uint32_t bt_diag_snapshot(char *out, uint32_t cap);

#endif /* BELLATRIX_IO_BT_DIAG_H */
