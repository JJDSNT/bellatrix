#include "io/hid/hid_router.h"

#include "io/hid/hid_amiga_map.h"
#include "machine/machine.h"

#include <stdatomic.h>
#include <string.h>

#define HID_USAGE_F11 0x44u
#define HID_USAGE_F12 0x45u
#define HID_ROUTER_MAX_DEVICES 16u

typedef struct HIDRouterDevice {
    bool active;
    HIDInputSource source;
    uint16_t id;
    uint8_t guest_keys[32];
    uint8_t host_keys[32];
    uint32_t guest_buttons;
    uint32_t host_buttons;
} HIDRouterDevice;

static HIDHostSink s_host_sink;
static _Atomic uint32_t s_host_action;
static HIDRouterDevice s_devices[HID_ROUTER_MAX_DEVICES];
static uint8_t s_guest_key_owners[256];
static uint8_t s_guest_button_owners[32];
static uint8_t s_hotkey_owners[2];

static bool bit_test(const uint8_t bits[32], uint8_t bit)
{
    return (bits[bit >> 3] & (uint8_t)(1u << (bit & 7u))) != 0u;
}

static void bit_set(uint8_t bits[32], uint8_t bit)
{
    bits[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
}

static void bit_clear(uint8_t bits[32], uint8_t bit)
{
    bits[bit >> 3] &= (uint8_t)~(1u << (bit & 7u));
}

static HIDRouterDevice *device_find(HIDInputSource source, uint16_t id,
                                    bool allocate)
{
    HIDRouterDevice *free_slot = NULL;
    for (unsigned i = 0u; i < HID_ROUTER_MAX_DEVICES; i++) {
        HIDRouterDevice *d = &s_devices[i];
        if (d->active && d->source == source && d->id == id)
            return d;
        if (!d->active && !free_slot)
            free_slot = d;
    }
    if (!allocate || !free_slot)
        return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = true;
    free_slot->source = source;
    free_slot->id = id;
    return free_slot;
}

void hid_router_init(void)
{
    memset(&s_host_sink, 0, sizeof(s_host_sink));
    memset(s_devices, 0, sizeof(s_devices));
    memset(s_guest_key_owners, 0, sizeof(s_guest_key_owners));
    memset(s_guest_button_owners, 0, sizeof(s_guest_button_owners));
    memset(s_hotkey_owners, 0, sizeof(s_hotkey_owners));
    atomic_store_explicit(&s_host_action, HID_HOST_ACTION_NONE,
                          memory_order_release);
}

void hid_router_set_host_sink(const HIDHostSink *sink)
{
    if (sink)
        s_host_sink = *sink;
    else
        memset(&s_host_sink, 0, sizeof(s_host_sink));
}

void hid_router_key(HIDInputSource source, uint16_t device,
                    uint8_t usage, bool pressed)
{
    HIDRouterDevice *d = device_find(source, device, pressed);
    unsigned hotkey = usage == HID_USAGE_F11 ? 0u :
                      usage == HID_USAGE_F12 ? 1u : 2u;

    if (!d)
        return;

    if (!pressed) {
        if (bit_test(d->host_keys, usage)) {
            bit_clear(d->host_keys, usage);
            if (hotkey < 2u && s_hotkey_owners[hotkey] != 0u)
                s_hotkey_owners[hotkey]--;
            if (s_host_sink.key)
                (void)s_host_sink.key(s_host_sink.ctx, source, device,
                                      usage, false);
            return;
        }
        if (bit_test(d->guest_keys, usage)) {
            bit_clear(d->guest_keys, usage);
            if (s_guest_key_owners[usage] != 0u &&
                --s_guest_key_owners[usage] == 0u) {
                uint8_t rawkey;
                if (hid_usage_to_amiga_raw(usage, &rawkey))
                    bellatrix_machine_keyboard_rawkey(rawkey, 0);
            }
        }
        return;
    }

    if (bit_test(d->host_keys, usage) || bit_test(d->guest_keys, usage))
        return;

    if (s_host_sink.key &&
        s_host_sink.key(s_host_sink.ctx, source, device, usage, true)) {
        bit_set(d->host_keys, usage);
        return;
    }

    if (hotkey < 2u) {
        bit_set(d->host_keys, usage);
        if (s_hotkey_owners[hotkey]++ == 0u) {
            uint32_t expected = HID_HOST_ACTION_NONE;
            uint32_t action = hotkey == 0u
                ? HID_HOST_ACTION_BTSCAN : HID_HOST_ACTION_MEDIA;
            (void)atomic_compare_exchange_strong_explicit(
                &s_host_action, &expected, action,
                memory_order_release, memory_order_relaxed);
        }
        return;
    }

    uint8_t rawkey;
    if (!hid_usage_to_amiga_raw(usage, &rawkey))
        return;
    bit_set(d->guest_keys, usage);
    if (s_guest_key_owners[usage]++ == 0u)
        bellatrix_machine_keyboard_rawkey(rawkey, 1);
}

void hid_router_mouse_motion(HIDInputSource source, uint16_t device,
                             int dx, int dy)
{
    if (s_host_sink.mouse_motion &&
        s_host_sink.mouse_motion(s_host_sink.ctx, source, device, dx, dy))
        return;
    bellatrix_machine_mouse_motion(0u, dx, dy);
}

void hid_router_mouse_button(HIDInputSource source, uint16_t device,
                             unsigned button, bool pressed)
{
    if (button >= 32u)
        return;
    HIDRouterDevice *d = device_find(source, device, pressed);
    if (!d)
        return;
    uint32_t mask = 1u << button;

    if (!pressed) {
        if (d->host_buttons & mask) {
            d->host_buttons &= ~mask;
            if (s_host_sink.mouse_button)
                (void)s_host_sink.mouse_button(s_host_sink.ctx, source,
                                               device, button, false);
            return;
        }
        if (d->guest_buttons & mask) {
            d->guest_buttons &= ~mask;
            if (s_guest_button_owners[button] != 0u &&
                --s_guest_button_owners[button] == 0u)
                bellatrix_machine_mouse_button(0u, button, 0);
        }
        return;
    }

    if ((d->host_buttons | d->guest_buttons) & mask)
        return;
    if (s_host_sink.mouse_button &&
        s_host_sink.mouse_button(s_host_sink.ctx, source, device,
                                 button, true)) {
        d->host_buttons |= mask;
        return;
    }
    d->guest_buttons |= mask;
    if (s_guest_button_owners[button]++ == 0u)
        bellatrix_machine_mouse_button(0u, button, 1);
}

void hid_router_device_disconnected(HIDInputSource source, uint16_t device)
{
    HIDRouterDevice *d = device_find(source, device, false);
    if (!d)
        return;

    for (unsigned usage = 0u; usage < 256u; usage++) {
        if (bit_test(d->guest_keys, (uint8_t)usage))
            hid_router_key(source, device, (uint8_t)usage, false);
        else if (bit_test(d->host_keys, (uint8_t)usage))
            hid_router_key(source, device, (uint8_t)usage, false);
    }
    for (unsigned button = 0u; button < 32u; button++) {
        uint32_t mask = 1u << button;
        if ((d->guest_buttons | d->host_buttons) & mask)
            hid_router_mouse_button(source, device, button, false);
    }
    memset(d, 0, sizeof(*d));
}

HIDHostAction hid_router_take_host_action(void)
{
    return (HIDHostAction)atomic_exchange_explicit(
        &s_host_action, HID_HOST_ACTION_NONE, memory_order_acq_rel);
}
