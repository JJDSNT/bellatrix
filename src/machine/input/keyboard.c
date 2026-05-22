#include "machine/input/keyboard.h"

#include "chipset/cia/cia.h"
#include "chipset/cia/cia_serial.h"

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
    if (kbd->waiting_handshake)
    {
        uint8_t sp = cia_serial_sp_output_level(cia);

        if (!sp)
            kbd->handshake_low_seen = 1u;
        else if (kbd->handshake_low_seen)
        {
            kbd->waiting_handshake = 0u;
            kbd->handshake_low_seen = 0u;
            kbd->handshake_timeout = 0u;
        }
        else if (kbd->handshake_timeout < 0xFFFFu)
        {
            kbd->handshake_timeout++;
        }

        if (kbd->handshake_timeout > 4096u)
        {
            kbd->waiting_handshake = 0u;
            kbd->handshake_low_seen = 0u;
            kbd->handshake_timeout = 0u;
        }

        if (kbd->waiting_handshake)
            return;
    }

    if (kbd->count == 0 || cia->sdr_full)
        return;

    if (!cia_serial_receive_byte(cia, kbd->queue[kbd->head]))
        return;

    /* The host keyboard is modeled as an external device that has already
     * serialized the wire byte. Keep the post-byte handshake so the guest
     * can acknowledge before the next queued event is delivered. */
    cia_set_sp_level(cia, 1u);
    cia_set_cnt_level(cia, 1u);
    kbd->head = (uint8_t)((kbd->head + 1u) % BELLATRIX_KEYBOARD_QUEUE_CAP);
    kbd->count--;
    kbd->waiting_handshake = 1u;
    kbd->handshake_low_seen = 0u;
    kbd->handshake_timeout = 0u;
}
