#ifndef BELLATRIX_IO_HID_AMIGA_MAP_H
#define BELLATRIX_IO_HID_AMIGA_MAP_H

/* HID Usage Page 0x07 (Keyboard/Keypad) → Amiga raw key codes.
 * Shared by the USB HID and Bluetooth HID paths so both feed the
 * same CIA-A keyboard queue with identical translation. */

#include <stdbool.h>
#include <stdint.h>

/* HID keyboard usage codes (subset used in Amiga mapping) */
#define HID_AMIGA_USAGE_NONE    0x00u
#define HID_AMIGA_USAGE_A       0x04u
#define HID_AMIGA_USAGE_Z       0x1Du
#define HID_AMIGA_USAGE_1       0x1Eu
#define HID_AMIGA_USAGE_0       0x27u
#define HID_AMIGA_USAGE_ENTER   0x28u
#define HID_AMIGA_USAGE_ESC     0x29u
#define HID_AMIGA_USAGE_BKSP    0x2Au
#define HID_AMIGA_USAGE_TAB     0x2Bu
#define HID_AMIGA_USAGE_SPACE   0x2Cu
#define HID_AMIGA_USAGE_HYPHEN  0x2Du
#define HID_AMIGA_USAGE_EQUAL   0x2Eu
#define HID_AMIGA_USAGE_LBRACE  0x2Fu
#define HID_AMIGA_USAGE_RBRACE  0x30u
#define HID_AMIGA_USAGE_BSLASH  0x31u
#define HID_AMIGA_USAGE_SEMI    0x33u
#define HID_AMIGA_USAGE_SQUOTE  0x34u
#define HID_AMIGA_USAGE_GRAVE   0x35u
#define HID_AMIGA_USAGE_COMMA   0x36u
#define HID_AMIGA_USAGE_PERIOD  0x37u
#define HID_AMIGA_USAGE_SLASH   0x38u
#define HID_AMIGA_USAGE_CAPS    0x39u
#define HID_AMIGA_USAGE_F1      0x3Au
#define HID_AMIGA_USAGE_F10     0x43u
#define HID_AMIGA_USAGE_DELFWD  0x4Cu
#define HID_AMIGA_USAGE_RIGHT   0x4Fu
#define HID_AMIGA_USAGE_LEFT    0x50u
#define HID_AMIGA_USAGE_DOWN    0x51u
#define HID_AMIGA_USAGE_UP      0x52u
#define HID_AMIGA_USAGE_KP1     0x59u
#define HID_AMIGA_USAGE_KP0     0x62u
#define HID_AMIGA_USAGE_LCTRL   0xE0u
#define HID_AMIGA_USAGE_LSHIFT  0xE1u
#define HID_AMIGA_USAGE_LALT    0xE2u
#define HID_AMIGA_USAGE_LGUI    0xE3u
#define HID_AMIGA_USAGE_RCTRL   0xE4u
#define HID_AMIGA_USAGE_RSHIFT  0xE5u
#define HID_AMIGA_USAGE_RALT    0xE6u
#define HID_AMIGA_USAGE_RGUI    0xE7u

/* HID modifier byte bit masks */
#define HID_AMIGA_MOD_LCTRL  0x01u
#define HID_AMIGA_MOD_LSHIFT 0x02u
#define HID_AMIGA_MOD_LALT   0x04u
#define HID_AMIGA_MOD_LGUI   0x08u
#define HID_AMIGA_MOD_RCTRL  0x10u
#define HID_AMIGA_MOD_RSHIFT 0x20u
#define HID_AMIGA_MOD_RALT   0x40u
#define HID_AMIGA_MOD_RGUI   0x80u

/* Map HID usage (page 0x07) to Amiga raw key code.
 * Returns true and fills *rawkey if the usage has an Amiga mapping. */
static inline bool hid_usage_to_amiga_raw(uint8_t usage, uint8_t *rawkey)
{
    if (usage >= 0x04u && usage <= 0x1Du) {
        static const uint8_t letters[] = {
            0x20u,0x35u,0x33u,0x22u,0x12u,0x23u,0x24u,0x25u,0x17u,
            0x26u,0x27u,0x28u,0x37u,0x36u,0x18u,0x19u,0x10u,0x13u,
            0x21u,0x14u,0x16u,0x34u,0x11u,0x32u,0x15u,0x31u
        };
        *rawkey = letters[usage - 0x04u];
        return true;
    }
    if (usage >= 0x1Eu && usage <= 0x27u) { *rawkey = (uint8_t)(usage - 0x1Du); return true; }
    if (usage >= 0x3Au && usage <= 0x43u) { *rawkey = (uint8_t)(0x50u + usage - 0x3Au); return true; } /* F1-F10 */
    if (usage >= 0x59u && usage <= 0x61u) {
        static const uint8_t kp[] = { 0x1Du,0x1Eu,0x1Fu,0x2Du,0x2Eu,0x2Fu,0x3Du,0x3Eu,0x3Fu };
        *rawkey = kp[usage - 0x59u];
        return true;
    }
    switch (usage) {
    case 0x28u: *rawkey = 0x44u; return true; /* Enter */
    case 0x29u: *rawkey = 0x45u; return true; /* Escape */
    case 0x2Au: *rawkey = 0x41u; return true; /* Backspace → Amiga DEL */
    case 0x2Bu: *rawkey = 0x42u; return true; /* Tab */
    case 0x2Cu: *rawkey = 0x40u; return true; /* Space */
    case 0x2Du: *rawkey = 0x0Bu; return true; /* - */
    case 0x2Eu: *rawkey = 0x0Cu; return true; /* = */
    case 0x2Fu: *rawkey = 0x1Au; return true; /* [ */
    case 0x30u: *rawkey = 0x1Bu; return true; /* ] */
    case 0x31u: *rawkey = 0x0Du; return true; /* \ */
    case 0x33u: *rawkey = 0x29u; return true; /* ; */
    case 0x34u: *rawkey = 0x2Au; return true; /* ' */
    case 0x35u: *rawkey = 0x00u; return true; /* ` */
    case 0x36u: *rawkey = 0x38u; return true; /* , */
    case 0x37u: *rawkey = 0x39u; return true; /* . */
    case 0x38u: *rawkey = 0x3Au; return true; /* / */
    case 0x39u: *rawkey = 0x62u; return true; /* Caps Lock */
    case 0x4Cu: *rawkey = 0x46u; return true; /* Forward Delete */
    case 0x4Fu: *rawkey = 0x4Eu; return true; /* Right */
    case 0x50u: *rawkey = 0x4Fu; return true; /* Left */
    case 0x51u: *rawkey = 0x4Du; return true; /* Down */
    case 0x52u: *rawkey = 0x4Cu; return true; /* Up */
    case 0x58u: *rawkey = 0x43u; return true; /* KP Enter */
    case 0x62u: *rawkey = 0x0Fu; return true; /* KP 0 */
    case 0xE0u: *rawkey = 0x63u; return true; /* L Ctrl */
    case 0xE1u: *rawkey = 0x60u; return true; /* L Shift */
    case 0xE2u: *rawkey = 0x64u; return true; /* L Alt */
    case 0xE3u: *rawkey = 0x66u; return true; /* L GUI (Amiga) */
    case 0xE4u: *rawkey = 0x63u; return true; /* R Ctrl */
    case 0xE5u: *rawkey = 0x61u; return true; /* R Shift */
    case 0xE6u: *rawkey = 0x65u; return true; /* R Alt */
    case 0xE7u: *rawkey = 0x67u; return true; /* R GUI (Amiga) */
    default:    return false;
    }
}

#endif /* BELLATRIX_IO_HID_AMIGA_MAP_H */
