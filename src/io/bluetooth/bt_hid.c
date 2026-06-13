#include "io/bluetooth/bt_hid.h"
#include "io/hid/hid_amiga_map.h"
#include "machine/machine.h"
#include <string.h>

/* Per-connection keyboard state: shadow of last report for diff-and-emit. */
#define BT_HID_MAX_CONNS 4u

typedef struct {
    uint16_t hid_cid;
    uint8_t  modifiers;
    uint8_t  keys[6];
    uint8_t  mouse_buttons;
} BTHIDConn;

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

static void emit_key(uint8_t usage, bool pressed)
{
    uint8_t rawkey;
    if (!hid_usage_to_amiga_raw(usage, &rawkey)) return;
    bellatrix_machine_keyboard_rawkey(rawkey, pressed ? 1 : 0);
}

static void emit_modifier_changes(uint8_t prev, uint8_t cur)
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
        if (was != is) emit_key(mods[i].usage, is);
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

    emit_modifier_changes(c->modifiers, modifier);

    for (unsigned i = 0u; i < 6u; i++) {
        uint8_t u = c->keys[i];
        if (u && !report_contains(keys, u)) emit_key(u, false);
    }
    for (unsigned i = 0u; i < 6u; i++) {
        uint8_t u = keys[i];
        if (u && !report_contains(c->keys, u)) emit_key(u, true);
    }

    c->modifiers = modifier;
    memcpy(c->keys, keys, 6u);
}

/* Boot protocol mouse report: byte0=buttons, byte1=xdisp(signed), byte2=ydisp(signed) */
void bt_hid_handle_mouse_report(uint16_t hid_cid,
                                const uint8_t *data, uint16_t len)
{
    if (!data || len < 3u) return;
    BTHIDConn *c = find_or_alloc_conn(hid_cid);
    if (!c) return;

    uint8_t buttons = data[0] & 0x07u;
    int dx = (int)(int8_t)data[1];
    int dy = (int)(int8_t)data[2];

    if (dx || dy)
        bellatrix_machine_mouse_motion(0u, dx, dy);

    for (unsigned btn = 0u; btn < 3u; btn++) {
        int was = (c->mouse_buttons >> btn) & 1;
        int is  = (buttons >> btn) & 1;
        if (was != is)
            bellatrix_machine_mouse_button(0u, btn, is);
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

    emit_modifier_changes(c->modifiers, 0u);
    for (unsigned i = 0u; i < 6u; i++)
        if (c->keys[i]) emit_key(c->keys[i], false);

    for (unsigned btn = 0u; btn < 3u; btn++)
        if ((c->mouse_buttons >> btn) & 1u)
            bellatrix_machine_mouse_button(0u, btn, 0);

    free_conn(hid_cid);
}
