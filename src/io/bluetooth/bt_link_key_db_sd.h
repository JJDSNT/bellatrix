/* bt_link_key_db_sd.h — btstack_link_key_db_t backed by BTKEYS.TXT on SD.
 *
 * Usage:
 *   At startup: bt_link_key_db_sd_load(text, len)  (pass BTKEYS.TXT contents)
 *   In bt_host: hci_set_link_key_db(bt_link_key_db_sd_instance())
 *   After pairing: bt_link_key_db_sd_save_if_dirty(fs)
 *
 * File format (one entry per line):
 *   AA:BB:CC:DD:EE:FF TT XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\r\n
 *   (MAC 17c) (type 2c hex) (key 32c hex) = 55 chars/line
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "classic/btstack_link_key_db.h"

#define BT_LINK_KEY_MAX      8u
#define BT_LINK_KEY_FILENAME "BTKEYS.TXT"
#define BT_LINK_KEY_FILE_CAP 512u

const btstack_link_key_db_t *bt_link_key_db_sd_instance(void);

void     bt_link_key_db_sd_load(const char *text, uint32_t len);
uint32_t bt_link_key_db_sd_serialise(char *buf, uint32_t cap);
bool     bt_link_key_db_sd_dirty(void);
void     bt_link_key_db_sd_clear_dirty(void);
