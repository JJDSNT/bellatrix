#include "machine/machine.h"
#include "io/hid/hid_amiga_map.h"
#include "support.h"

#ifdef BELLATRIX_LAUNCHER
#include "launcher/launcher_input.h"
#endif

#if BELLATRIX_ENABLE_USBSTACK

#include "usb_config.h"
#include "usb_errno.h"
#include "usbh_core.h"
#include "usbh_hid.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define BELLATRIX_USB_HID_MAX_CONTEXTS CONFIG_USBHOST_MAX_HID_CLASS
#define BELLATRIX_USB_HID_REPORT_BYTES 64u

typedef struct BellatrixUSBHIDKeyboard {
    bool active;
    uint8_t minor;
    uint8_t modifiers;
    uint8_t keys[6];
} BellatrixUSBHIDKeyboard;

typedef struct BellatrixUSBHIDMouse {
    bool active;
    uint8_t minor;
    uint8_t buttons;
} BellatrixUSBHIDMouse;

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t
    g_bellatrix_usb_hid_report[BELLATRIX_USB_HID_MAX_CONTEXTS][USB_ALIGN_UP(BELLATRIX_USB_HID_REPORT_BYTES, CONFIG_USB_ALIGN_SIZE)];
static BellatrixUSBHIDKeyboard g_bellatrix_usb_hid_keyboards[BELLATRIX_USB_HID_MAX_CONTEXTS];
static BellatrixUSBHIDMouse    g_bellatrix_usb_hid_mice[BELLATRIX_USB_HID_MAX_CONTEXTS];

static BellatrixUSBHIDKeyboard *bellatrix_usb_hid_keyboard_for_minor(uint8_t minor)
{
    if (minor >= BELLATRIX_USB_HID_MAX_CONTEXTS) {
        return NULL;
    }
    return &g_bellatrix_usb_hid_keyboards[minor];
}

static BellatrixUSBHIDMouse *bellatrix_usb_hid_mouse_for_minor(uint8_t minor)
{
    if (minor >= BELLATRIX_USB_HID_MAX_CONTEXTS) {
        return NULL;
    }
    return &g_bellatrix_usb_hid_mice[minor];
}

/* Thin wrapper kept for call-site compatibility inside this file. */
static bool bellatrix_usb_hid_usage_to_amiga_raw(uint8_t usage, uint8_t *rawkey)
{
    if (!rawkey) return false;
    return hid_usage_to_amiga_raw(usage, rawkey);
}

static bool bellatrix_usb_hid_report_contains(const uint8_t keys[6], uint8_t usage)
{
    unsigned int i;

    for (i = 0; i < 6u; ++i) {
        if (keys[i] == usage) {
            return true;
        }
    }

    return false;
}

static void bellatrix_usb_hid_emit_usage(uint8_t usage, bool pressed)
{
    uint8_t rawkey;

    if (usage == HID_KBD_USAGE_NONE) {
        return;
    }

#ifdef BELLATRIX_LAUNCHER
    if (pressed && launcher_input_is_active()) {
        launcher_input_push(usage);
        return;
    }
#endif

    if (bellatrix_usb_hid_usage_to_amiga_raw(usage, &rawkey)) {
        kprintf("[HID->AMIGA] usage=0x%02x %s rawkey=0x%02x\n",
                (unsigned)usage, pressed ? "down" : "up", (unsigned)rawkey);
        bellatrix_machine_keyboard_rawkey(rawkey, pressed ? 1 : 0);
    } else {
        kprintf("[HID->AMIGA] usage=0x%02x %s (no amiga mapping)\n",
                (unsigned)usage, pressed ? "down" : "up");
    }
}

static void bellatrix_usb_hid_emit_modifier_changes(uint8_t previous, uint8_t current)
{
    const struct {
        uint8_t mask;
        uint8_t usage;
    } modifiers[] = {
        { HID_MODIFIER_LCTRL, HID_KBD_USAGE_LCTRL },
        { HID_MODIFIER_LSHIFT, HID_KBD_USAGE_LSHIFT },
        { HID_MODIFIER_LALT, HID_KBD_USAGE_LALT },
        { HID_MODIFIER_LGUI, HID_KBD_USAGE_LGUI },
        { HID_MODIFIER_RCTRL, HID_KBD_USAGE_RCTRL },
        { HID_MODIFIER_RSHIFT, HID_KBD_USAGE_RSHIFT },
        { HID_MODIFIER_RALT, HID_KBD_USAGE_RALT },
        { HID_MODIFIER_RGUI, HID_KBD_USAGE_RGUI },
    };
    unsigned int i;

    for (i = 0; i < (sizeof(modifiers) / sizeof(modifiers[0])); ++i) {
        bool was_pressed = (previous & modifiers[i].mask) != 0u;
        bool is_pressed = (current & modifiers[i].mask) != 0u;

        if (was_pressed != is_pressed) {
            bellatrix_usb_hid_emit_usage(modifiers[i].usage, is_pressed);
        }
    }
}

static void bellatrix_usb_hid_release_all(BellatrixUSBHIDKeyboard *keyboard)
{
    unsigned int i;

    if (!keyboard) {
        return;
    }

    bellatrix_usb_hid_emit_modifier_changes(keyboard->modifiers, 0u);
    for (i = 0; i < 6u; ++i) {
        bellatrix_usb_hid_emit_usage(keyboard->keys[i], false);
    }

    keyboard->modifiers = 0u;
    memset(keyboard->keys, 0, sizeof(keyboard->keys));
}

static void bellatrix_usb_hid_resubmit(struct usbh_hid *hid_class,
                                       uint8_t *buffer,
                                       uint32_t buffer_len,
                                       void (*callback)(void *arg, int nbytes))
{
    usbh_int_urb_fill(&hid_class->intin_urb,
                      hid_class->hport,
                      hid_class->intin,
                      buffer,
                      buffer_len,
                      0,
                      callback,
                      hid_class);
    usbh_submit_urb(&hid_class->intin_urb);
}

static uint32_t bellatrix_usb_hid_transfer_len(const struct usbh_hid *hid_class)
{
    uint32_t transfer_len = BELLATRIX_USB_HID_REPORT_BYTES;

    if (hid_class && hid_class->intin && hid_class->intin->wMaxPacketSize > 0u &&
        hid_class->intin->wMaxPacketSize < transfer_len) {
        transfer_len = hid_class->intin->wMaxPacketSize;
    }

    return transfer_len;
}

static void bellatrix_usb_hid_keyboard_callback(void *arg, int nbytes)
{
    struct usbh_hid *hid_class = (struct usbh_hid *)arg;
    BellatrixUSBHIDKeyboard *keyboard;
    struct usb_hid_kbd_report *report;
    unsigned int i;

    if (!hid_class) {
        return;
    }

    keyboard = (BellatrixUSBHIDKeyboard *)hid_class->user_data;
    if (!keyboard) {
        return;
    }

    if (nbytes >= (int)sizeof(struct usb_hid_kbd_report)) {
        report = (struct usb_hid_kbd_report *)g_bellatrix_usb_hid_report[hid_class->minor];

        bellatrix_usb_hid_emit_modifier_changes(keyboard->modifiers, report->modifier);

        for (i = 0; i < 6u; ++i) {
            uint8_t usage = keyboard->keys[i];
            if (usage != HID_KBD_USAGE_NONE && !bellatrix_usb_hid_report_contains(report->key, usage)) {
                bellatrix_usb_hid_emit_usage(usage, false);
            }
        }

        for (i = 0; i < 6u; ++i) {
            uint8_t usage = report->key[i];
            if (usage != HID_KBD_USAGE_NONE && !bellatrix_usb_hid_report_contains(keyboard->keys, usage)) {
                bellatrix_usb_hid_emit_usage(usage, true);
            }
        }

        keyboard->modifiers = report->modifier;
        for (i = 0; i < 6u; ++i) {
            keyboard->keys[i] = report->key[i];
        }
    }

    /* Interrupt endpoints must always be re-armed regardless of error code.
     * Any error other than NAK (e.g. timeout, pipe stall) would otherwise
     * silently kill the callback chain on real hardware. */
    bellatrix_usb_hid_resubmit(hid_class,
                               g_bellatrix_usb_hid_report[hid_class->minor],
                               bellatrix_usb_hid_transfer_len(hid_class),
                               bellatrix_usb_hid_keyboard_callback);
}

static void bellatrix_usb_hid_mouse_callback(void *arg, int nbytes)
{
    struct usbh_hid *hid_class = (struct usbh_hid *)arg;
    BellatrixUSBHIDMouse *mouse;
    struct usb_hid_mouse_report *report;
    unsigned int btn;

    if (!hid_class) {
        return;
    }

    mouse = (BellatrixUSBHIDMouse *)hid_class->user_data;
    if (!mouse) {
        return;
    }

    if (nbytes > 0) {
        report = (struct usb_hid_mouse_report *)g_bellatrix_usb_hid_report[hid_class->minor];

        if ((report->buttons & 0x07u) != mouse->buttons ||
            report->xdisp || report->ydisp) {
            kprintf("[USB-HID-MOUSE] minor=%u buttons=%02x->%02x dx=%d dy=%d nbytes=%d\n",
                    (unsigned)hid_class->minor,
                    (unsigned)(mouse->buttons & 0x07u),
                    (unsigned)(report->buttons & 0x07u),
                    (int)report->xdisp,
                    (int)report->ydisp,
                    nbytes);
        }

        if (report->xdisp || report->ydisp) {
            bellatrix_machine_mouse_motion(0u,
                                           (int)report->xdisp,
                                           (int)report->ydisp);
        }

        for (btn = 0; btn < 3u; ++btn) {
            int was = (mouse->buttons >> btn) & 1;
            int is  = (report->buttons >> btn) & 1;
            if (was != is) {
                bellatrix_machine_mouse_button(0u, btn, is);
            }
        }

        mouse->buttons = report->buttons & 0x07u;
    }

    bellatrix_usb_hid_resubmit(hid_class,
                               g_bellatrix_usb_hid_report[hid_class->minor],
                               bellatrix_usb_hid_transfer_len(hid_class),
                               bellatrix_usb_hid_mouse_callback);
}

void usbh_hid_run(struct usbh_hid *hid_class)
{
    uint8_t protocol;

    if (!hid_class || !hid_class->intin) {
        return;
    }

    protocol = hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc.bInterfaceProtocol;

    if (protocol == HID_PROTOCOL_KEYBOARD) {
        BellatrixUSBHIDKeyboard *keyboard = bellatrix_usb_hid_keyboard_for_minor(hid_class->minor);
        if (!keyboard) {
            kprintf("[USB-HID] no keyboard context for HID minor %u\n", (unsigned)hid_class->minor);
            return;
        }

        memset(keyboard, 0, sizeof(*keyboard));
        keyboard->active = true;
        keyboard->minor = hid_class->minor;
        hid_class->user_data = keyboard;

        usbh_hid_set_protocol(hid_class, HID_PROTOCOL_BOOT);
        kprintf("[USB-HID] keyboard attached: minor=%u intf=%u ep_in=0x%02x mps=%u\n",
                (unsigned)hid_class->minor,
                (unsigned)hid_class->intf,
                (unsigned)hid_class->intin->bEndpointAddress,
                (unsigned)hid_class->intin->wMaxPacketSize);

        memset(g_bellatrix_usb_hid_report[hid_class->minor], 0, sizeof(g_bellatrix_usb_hid_report[hid_class->minor]));
        bellatrix_usb_hid_resubmit(hid_class,
                                   g_bellatrix_usb_hid_report[hid_class->minor],
                                   bellatrix_usb_hid_transfer_len(hid_class),
                                   bellatrix_usb_hid_keyboard_callback);

    } else if (protocol == HID_PROTOCOL_MOUSE) {
        BellatrixUSBHIDMouse *mouse = bellatrix_usb_hid_mouse_for_minor(hid_class->minor);
        if (!mouse) {
            kprintf("[USB-HID] no mouse context for HID minor %u\n", (unsigned)hid_class->minor);
            return;
        }

        memset(mouse, 0, sizeof(*mouse));
        mouse->active = true;
        mouse->minor = hid_class->minor;
        hid_class->user_data = mouse;

        usbh_hid_set_protocol(hid_class, HID_PROTOCOL_BOOT);
        kprintf("[USB-HID] mouse attached: minor=%u intf=%u ep_in=0x%02x mps=%u\n",
                (unsigned)hid_class->minor,
                (unsigned)hid_class->intf,
                (unsigned)hid_class->intin->bEndpointAddress,
                (unsigned)hid_class->intin->wMaxPacketSize);

        memset(g_bellatrix_usb_hid_report[hid_class->minor], 0, sizeof(g_bellatrix_usb_hid_report[hid_class->minor]));
        bellatrix_usb_hid_resubmit(hid_class,
                                   g_bellatrix_usb_hid_report[hid_class->minor],
                                   bellatrix_usb_hid_transfer_len(hid_class),
                                   bellatrix_usb_hid_mouse_callback);
    }
}

void usbh_hid_stop(struct usbh_hid *hid_class)
{
    uint8_t protocol;

    if (!hid_class) {
        return;
    }

    protocol = hid_class->hport->config.intf[hid_class->intf].altsetting[0].intf_desc.bInterfaceProtocol;

    if (protocol == HID_PROTOCOL_KEYBOARD) {
        BellatrixUSBHIDKeyboard *keyboard = (BellatrixUSBHIDKeyboard *)hid_class->user_data;
        if (keyboard) {
            bellatrix_usb_hid_release_all(keyboard);
            keyboard->active = false;
            hid_class->user_data = NULL;
            kprintf("[USB-HID] keyboard detached: minor=%u\n", (unsigned)hid_class->minor);
        }
    } else if (protocol == HID_PROTOCOL_MOUSE) {
        BellatrixUSBHIDMouse *mouse = (BellatrixUSBHIDMouse *)hid_class->user_data;
        if (mouse) {
            BellatrixMachine *m = bellatrix_machine_get();
            unsigned int btn;
            for (btn = 0; btn < 3u; ++btn) {
                if ((mouse->buttons >> btn) & 1) {
                    bellatrix_controller_port_set_mouse_button(&m->controller_ports,
                                                               BELLATRIX_CONTROLLER_PORT_MOUSE,
                                                               btn, 0);
                }
            }
            mouse->active = false;
            hid_class->user_data = NULL;
            kprintf("[USB-HID] mouse detached: minor=%u\n", (unsigned)hid_class->minor);
        }
    }
}

#endif
