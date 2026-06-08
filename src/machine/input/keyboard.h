#ifndef BELLATRIX_INPUT_KEYBOARD_H
#define BELLATRIX_INPUT_KEYBOARD_H

#include <stdint.h>

#define BELLATRIX_KEYBOARD_QUEUE_CAP 64u

typedef struct BellatrixKeyboard {
    uint8_t queue[BELLATRIX_KEYBOARD_QUEUE_CAP];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    uint8_t tx_byte;
    uint8_t tx_bits_remaining;
    uint8_t tx_clock_low;
    uint8_t waiting_handshake;
    uint8_t handshake_low_seen;
    uint16_t handshake_timeout;
} BellatrixKeyboard;

void bellatrix_keyboard_init(BellatrixKeyboard *kbd);
void bellatrix_keyboard_reset(BellatrixKeyboard *kbd);
int  bellatrix_keyboard_enqueue_byte(BellatrixKeyboard *kbd, uint8_t byte);
int  bellatrix_keyboard_enqueue_raw(BellatrixKeyboard *kbd, uint8_t rawkey, int pressed);

#endif
