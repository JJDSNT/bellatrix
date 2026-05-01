#include "input/keyboard.h"

#include "chipset/cia/cia.h"

#include <string.h>

static uint8_t bellatrix_keyboard_encode_wire_byte(uint8_t raw)
{
    /* Amiga keyboard bytes arrive pre-encoded on the CIA serial path.
     * DiagROM decodes them with ROR #1 + NOT, so queue the inverse form here. */
    uint8_t inverted = (uint8_t)~raw;
    return (uint8_t)((uint8_t)(inverted << 1) | (inverted >> 7));
}

void bellatrix_keyboard_init(BellatrixKeyboard *kbd)
{
    memset(kbd, 0, sizeof(*kbd));
}

void bellatrix_keyboard_reset(BellatrixKeyboard *kbd)
{
    bellatrix_keyboard_init(kbd);
}

int bellatrix_keyboard_enqueue_byte(BellatrixKeyboard *kbd, uint8_t byte)
{
    if (kbd->count >= BELLATRIX_KEYBOARD_QUEUE_CAP)
        return 0;

    kbd->queue[kbd->tail] = byte;
    kbd->tail = (uint8_t)((kbd->tail + 1u) % BELLATRIX_KEYBOARD_QUEUE_CAP);
    kbd->count++;
    return 1;
}

int bellatrix_keyboard_enqueue_raw(BellatrixKeyboard *kbd, uint8_t rawkey, int pressed)
{
    uint8_t raw = (uint8_t)(rawkey & 0x7Fu);

    if (!pressed)
        raw |= 0x80u;

    return bellatrix_keyboard_enqueue_byte(
        kbd, bellatrix_keyboard_encode_wire_byte(raw));
}

void bellatrix_keyboard_step(BellatrixKeyboard *kbd, struct CIA_State *cia)
{
    if (kbd->count == 0)
        return;

    if (!cia_receive_sdr(cia, kbd->queue[kbd->head]))
        return;

    kbd->head = (uint8_t)((kbd->head + 1u) % BELLATRIX_KEYBOARD_QUEUE_CAP);
    kbd->count--;
}
