#ifndef BELLATRIX_IO_BT_PAIRS_H
#define BELLATRIX_IO_BT_PAIRS_H

#include <stdbool.h>
#include <stdint.h>
#include "io/bluetooth/bt_scan.h"

/* Persistent list of preferred BT devices saved to SD as BTPAIRS.TXT.
 * File format (one device per line):
 *   CL K AA:BB:CC:DD:EE:FF device name
 * Transport tag: CL/LE/DM.  Device type: K=keyboard M=mouse J=joystick ?=unknown.
 * The file must already exist on the SD (any size ≥ capacity needed). */

#define BT_PAIRS_MAX      8u
#define BT_PAIRS_FILENAME "BTPAIRS.TXT"
/* Reserve 512 bytes on the SD: enough for 8 entries + padding. */
#define BT_PAIRS_FILE_CAP 512u

/* Device type codes (stored as single char in BTPAIRS.TXT) */
#define BT_PAIRS_TYPE_UNKNOWN   0u  /* '?' */
#define BT_PAIRS_TYPE_KEYBOARD  1u  /* 'K' */
#define BT_PAIRS_TYPE_MOUSE     2u  /* 'M' */
#define BT_PAIRS_TYPE_JOYSTICK  3u  /* 'J' */

typedef struct {
    uint8_t addr[6];
    uint8_t transport;              /* BT_SCAN_TRANSPORT_* bitmask */
    uint8_t device_type;            /* BT_PAIRS_TYPE_* */
    char    name[BT_SCAN_NAME_LEN];
} BTPair;

/* Parse a BTPAIRS.TXT text buffer.  Call with NULL/0 to reset. */
void          bt_pairs_load(const char *text, uint32_t len);

unsigned      bt_pairs_count(void);
const BTPair *bt_pairs_get(unsigned i);
bool          bt_pairs_is_known(const uint8_t addr[6]);

/* Add/update from a scan result.  Returns true if the list changed. */
bool          bt_pairs_add(const BTScanResult *r);

/* Remove by MAC.  Returns true if found and removed. */
bool          bt_pairs_remove(const uint8_t addr[6]);

/* True if the list has been modified since the last bt_pairs_load(). */
bool          bt_pairs_dirty(void);

/* Serialise to text.  Returns bytes written (not null-terminated). */
uint32_t      bt_pairs_serialise(char *buf, uint32_t cap);

#endif /* BELLATRIX_IO_BT_PAIRS_H */
