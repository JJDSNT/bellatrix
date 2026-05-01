#ifndef BELLATRIX_INPUT_KEYBOARD_H
#define BELLATRIX_INPUT_KEYBOARD_H

#include <stdint.h>

struct CIA_State;

#define BELLATRIX_KEYBOARD_QUEUE_CAP 64u

typedef struct BellatrixKeyboard {
    uint8_t queue[BELLATRIX_KEYBOARD_QUEUE_CAP];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} BellatrixKeyboard;

void bellatrix_keyboard_init(BellatrixKeyboard *kbd);
void bellatrix_keyboard_reset(BellatrixKeyboard *kbd);
int  bellatrix_keyboard_enqueue_byte(BellatrixKeyboard *kbd, uint8_t byte);
int  bellatrix_keyboard_enqueue_raw(BellatrixKeyboard *kbd, uint8_t rawkey, int pressed);
void bellatrix_keyboard_step(BellatrixKeyboard *kbd, struct CIA_State *cia);

#endif
