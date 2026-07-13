#ifndef BELLATRIX_IO_HID_ROUTER_H
#define BELLATRIX_IO_HID_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum HIDInputSource {
    HID_INPUT_USB = 0,
    HID_INPUT_BLUETOOTH,
} HIDInputSource;

typedef enum HIDHostAction {
    HID_HOST_ACTION_NONE = 0,
    HID_HOST_ACTION_BTSCAN,
    HID_HOST_ACTION_MEDIA,
} HIDHostAction;

/* Optional host-UI sink. Returning true consumes the event; returning false
 * lets it continue to the Amiga sink. Transport drivers never know which UI
 * is active and host UI never knows which transport produced the event. */
typedef struct HIDHostSink {
    bool (*key)(void *ctx, HIDInputSource source, uint16_t device,
                uint8_t usage, bool pressed);
    bool (*mouse_motion)(void *ctx, HIDInputSource source, uint16_t device,
                         int dx, int dy);
    bool (*mouse_button)(void *ctx, HIDInputSource source, uint16_t device,
                         unsigned button, bool pressed);
    void *ctx;
} HIDHostSink;

void hid_router_init(void);
void hid_router_set_host_sink(const HIDHostSink *sink);

void hid_router_key(HIDInputSource source, uint16_t device,
                    uint8_t usage, bool pressed);
void hid_router_mouse_motion(HIDInputSource source, uint16_t device,
                             int dx, int dy);
void hid_router_mouse_button(HIDInputSource source, uint16_t device,
                             unsigned button, bool pressed);
void hid_router_device_disconnected(HIDInputSource source, uint16_t device);

/* F11/F12 publish intent only. Core 0 consumes it after the current host
 * reactor pass; no UI or storage work runs inside a HID callback. */
HIDHostAction hid_router_take_host_action(void);

#endif
