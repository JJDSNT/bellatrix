#ifndef BELLATRIX_IO_BT_SCAN_H
#define BELLATRIX_IO_BT_SCAN_H

/* Dual-mode (Classic inquiry + LE active scan) device discovery.
 *
 * Diagnostic/pairing front-end: collects nearby devices so the launcher can
 * show them on the framebuffer — the serial console is dark while BTStack
 * owns the PL011, so the screen is the only place these results can go.
 *
 * Phases alternate (inquiry ≈5s → remote-name requests → LE scan 5s → …)
 * because the CYW43438 shares one radio between BR/EDR and LE.
 */

#include <stdbool.h>
#include <stdint.h>

#define BT_SCAN_MAX_RESULTS 12u
#define BT_SCAN_NAME_LEN    25u

#define BT_SCAN_TRANSPORT_CLASSIC 1u
#define BT_SCAN_TRANSPORT_LE      2u

typedef struct BTScanResult {
    uint8_t  addr[6];          /* big-endian, as printed */
    uint8_t  addr_type;        /* LE only: 0=public 1=random */
    uint8_t  transport;        /* BT_SCAN_TRANSPORT_* bitmask — dual-mode
                                  devices (public addr) merge into one entry */
    int8_t   rssi;
    uint32_t cod;              /* Classic only: class of device */
    char     name[BT_SCAN_NAME_LEN];   /* "" until known */
    /* Classic remote-name-request bookkeeping */
    uint8_t  psrm;
    uint16_t clock_offset;
    uint8_t  name_state;       /* 0=none 1=synthesized label 2=real name */
    bool     hid;              /* HID device (CoD peripheral / LE 0x1812) */
    uint16_t appearance;       /* LE appearance AD, 0 if absent */
} BTScanResult;

void bt_scan_start(void);
void bt_scan_stop(void);

unsigned            bt_scan_count(void);
const BTScanResult *bt_scan_get(unsigned index);

/* Short status line for the UI ("inquiry...", "LE scan...", ...). */
const char *bt_scan_status(void);

/* Increments whenever results/status change — cheap redraw check. */
uint32_t bt_scan_generation(void);

#endif /* BELLATRIX_IO_BT_SCAN_H */
