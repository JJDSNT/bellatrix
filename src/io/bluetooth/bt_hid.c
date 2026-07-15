#include "io/bluetooth/bt_hid.h"
#include "io/bluetooth/bt_diag.h"
#include "io/hid/hid_amiga_map.h"
#include "io/hid/hid_router.h"
#include "machine/machine.h"
#include <string.h>

/* Per-connection keyboard state: shadow of last report for diff-and-emit. */
#define BT_HID_MAX_CONNS 4u

typedef struct {
    uint16_t hid_cid;
    uint8_t  modifiers;
    uint8_t  keys[6];
    uint8_t  mouse_buttons;
    uint8_t  mouse_report_layout;
} BTHIDConn;

enum {
    BT_MOUSE_LAYOUT_UNKNOWN = 0,
    BT_MOUSE_LAYOUT_BOOT_8,
    BT_MOUSE_LAYOUT_ID1_16LE,
};

static BTHIDConn s_conns[BT_HID_MAX_CONNS];
static unsigned  s_conn_count;

void bt_hid_init(void)
{
    memset(s_conns, 0, sizeof(s_conns));
    s_conn_count = 0u;
}

static BTHIDConn *find_conn(uint16_t hid_cid)
{
    for (unsigned i = 0u; i < s_conn_count; i++)
        if (s_conns[i].hid_cid == hid_cid) return &s_conns[i];
    return NULL;
}

static BTHIDConn *find_or_alloc_conn(uint16_t hid_cid)
{
    BTHIDConn *c = find_conn(hid_cid);
    if (c) return c;
    if (s_conn_count >= BT_HID_MAX_CONNS) return NULL;
    c = &s_conns[s_conn_count++];
    memset(c, 0, sizeof(*c));
    c->hid_cid = hid_cid;
    return c;
}

static void free_conn(uint16_t hid_cid)
{
    for (unsigned i = 0u; i < s_conn_count; i++) {
        if (s_conns[i].hid_cid != hid_cid) continue;
        s_conns[i] = s_conns[--s_conn_count];
        return;
    }
}

static void emit_key(uint16_t hid_cid, uint8_t usage, bool pressed)
{
    hid_router_key(HID_INPUT_BLUETOOTH, hid_cid, usage, pressed);
}

static void emit_modifier_changes(uint16_t hid_cid, uint8_t prev, uint8_t cur)
{
    static const struct { uint8_t mask; uint8_t usage; } mods[] = {
        { HID_AMIGA_MOD_LCTRL,  0xE0u }, { HID_AMIGA_MOD_LSHIFT, 0xE1u },
        { HID_AMIGA_MOD_LALT,   0xE2u }, { HID_AMIGA_MOD_LGUI,   0xE3u },
        { HID_AMIGA_MOD_RCTRL,  0xE4u }, { HID_AMIGA_MOD_RSHIFT, 0xE5u },
        { HID_AMIGA_MOD_RALT,   0xE6u }, { HID_AMIGA_MOD_RGUI,   0xE7u },
    };
    for (unsigned i = 0u; i < 8u; i++) {
        bool was = (prev & mods[i].mask) != 0u;
        bool is  = (cur  & mods[i].mask) != 0u;
        if (was != is) emit_key(hid_cid, mods[i].usage, is);
    }
}

static bool report_contains(const uint8_t keys[6], uint8_t usage)
{
    for (unsigned i = 0u; i < 6u; i++)
        if (keys[i] == usage) return true;
    return false;
}

/* Boot protocol keyboard report: byte0=modifiers, byte1=reserved, bytes2-7=keys */
void bt_hid_handle_keyboard_report(uint16_t hid_cid,
                                   const uint8_t *data, uint16_t len)
{
    if (!data || len < 8u) return;
    BTHIDConn *c = find_or_alloc_conn(hid_cid);
    if (!c) return;

    uint8_t modifier = data[0];
    const uint8_t *keys = data + 2u; /* skip reserved byte */

    /* Boot-protocol error report (ErrorRollOver/POSTFail/ErrorUndefined in
     * the key array): ignore instead of diffing phantom key events. */
    for (unsigned i = 0u; i < 6u; i++)
        if (keys[i] >= 0x01u && keys[i] <= 0x03u) return;

    emit_modifier_changes(hid_cid, c->modifiers, modifier);

    for (unsigned i = 0u; i < 6u; i++) {
        uint8_t u = c->keys[i];
        if (u && !report_contains(keys, u)) emit_key(hid_cid, u, false);
    }
    for (unsigned i = 0u; i < 6u; i++) {
        uint8_t u = keys[i];
        if (u && !report_contains(c->keys, u)) emit_key(hid_cid, u, true);
    }

    c->modifiers = modifier;
    memcpy(c->keys, keys, 6u);
}

static bool decode_mouse_report(BTHIDConn *c, const uint8_t *data, uint16_t len,
                                uint8_t *buttons_out, int *dx_out, int *dy_out)
{
    if (!c || !data || !buttons_out || !dx_out || !dy_out)
        return false;

    uint8_t layout = c->mouse_report_layout;
    if (layout == BT_MOUSE_LAYOUT_UNKNOWN) {
        if (len >= 7u && data[0] == 0x01u)
            layout = BT_MOUSE_LAYOUT_ID1_16LE;
        else if (len >= 3u)
            layout = BT_MOUSE_LAYOUT_BOOT_8;
        else
            return false;
        c->mouse_report_layout = layout;
        bt_diag_log("[BT] mouse report layout: %s\n",
                    layout == BT_MOUSE_LAYOUT_ID1_16LE
                        ? "id1/buttons/x16le/y16le"
                        : "boot/buttons/x8/y8");
    }

    if (layout == BT_MOUSE_LAYOUT_ID1_16LE) {
        if (len < 7u || data[0] != 0x01u)
            return false;
        uint16_t x_raw = (uint16_t)data[2] | ((uint16_t)data[3] << 8);
        uint16_t y_raw = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
        *buttons_out = data[1] & 0x07u;
        *dx_out = (int)(int16_t)x_raw;
        *dy_out = (int)(int16_t)y_raw;
        return true;
    }

    if (len < 3u)
        return false;
    *buttons_out = data[0] & 0x07u;
    *dx_out = (int)(int8_t)data[1];
    *dy_out = (int)(int8_t)data[2];
    return true;
}

/* Decode boot reports and the report-ID/16-bit layout observed on Classic
 * HID mice whose descriptor is unavailable. */
void bt_hid_handle_mouse_report(uint16_t hid_cid,
                                const uint8_t *data, uint16_t len)
{
    if (!data || len == 0u) return;
    BTHIDConn *c = find_or_alloc_conn(hid_cid);
    if (!c) return;

    uint8_t buttons;
    int dx;
    int dy;
    if (!decode_mouse_report(c, data, len, &buttons, &dx, &dy)) {
        bt_diag_log("[BT-HID] ignored mouse report cid=0x%04x len=%u "
                    "id=0x%02x layout=%u\n",
                    (unsigned)hid_cid, (unsigned)len, (unsigned)data[0],
                    (unsigned)c->mouse_report_layout);
        return;
    }

    if (dx || dy)
        hid_router_mouse_motion(HID_INPUT_BLUETOOTH, hid_cid, dx, dy);

    for (unsigned btn = 0u; btn < 3u; btn++) {
        int was = (c->mouse_buttons >> btn) & 1;
        int is  = (buttons >> btn) & 1;
        if (was != is)
            hid_router_mouse_button(HID_INPUT_BLUETOOTH, hid_cid,
                                    btn, is != 0);
    }

    c->mouse_buttons = buttons;
}

/* Simple gamepad report heuristic: byte0=buttons, byte1=X axis, byte2=Y axis.
 * Treats ±64 threshold as digital direction. */
void bt_hid_handle_joystick_report(uint16_t hid_cid,
                                   const uint8_t *data, uint16_t len)
{
    if (!data || len < 3u) return;
    BTHIDConn *c = find_or_alloc_conn(hid_cid);
    if (!c) return;

    uint8_t buttons = data[0];
    int x = (int)(int8_t)data[1];
    int y = (int)(int8_t)data[2];

    /* directions */
    bellatrix_machine_joystick_direction(0u, 3u, x >  64); /* right */
    bellatrix_machine_joystick_direction(0u, 2u, x < -64); /* left */
    bellatrix_machine_joystick_direction(0u, 0u, y < -64); /* up */
    bellatrix_machine_joystick_direction(0u, 1u, y >  64); /* down */

    /* fire buttons */
    for (unsigned btn = 0u; btn < 3u; btn++) {
        int was = (c->mouse_buttons >> btn) & 1;
        int is  = (buttons >> btn) & 1;
        if (was != is)
            bellatrix_machine_joystick_button(0u, btn, is);
    }

    c->mouse_buttons = buttons; /* reuse field for joystick button state */
    (void)hid_cid;
}

void bt_hid_release_all(uint16_t hid_cid)
{
    BTHIDConn *c = find_conn(hid_cid);
    if (!c) return;

    emit_modifier_changes(hid_cid, c->modifiers, 0u);
    for (unsigned i = 0u; i < 6u; i++)
        if (c->keys[i]) emit_key(hid_cid, c->keys[i], false);

    for (unsigned btn = 0u; btn < 3u; btn++)
        if ((c->mouse_buttons >> btn) & 1u)
            hid_router_mouse_button(HID_INPUT_BLUETOOTH, hid_cid,
                                    btn, false);

    hid_router_device_disconnected(HID_INPUT_BLUETOOTH, hid_cid);
    free_conn(hid_cid);
}
