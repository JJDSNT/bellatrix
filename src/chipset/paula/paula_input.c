#include "chipset/paula/paula_input.h"

#include "debug/cpu_pc.h"
#include "support.h"

#include <string.h>

void paula_input_init(PaulaInput *input)
{
    memset(input, 0, sizeof(*input));
    paula_input_reset(input);
}

void paula_input_reset(PaulaInput *input)
{
    input->potgo = 0xFFFFu;
    input->joydat[0] = 0u;
    input->joydat[1] = 0u;
    input->pot_button_x[0] = 0u;
    input->pot_button_x[1] = 0u;
    input->pot_button_y[0] = 0u;
    input->pot_button_y[1] = 0u;
    input->pot_button_x_seen[0] = 0u;
    input->pot_button_x_seen[1] = 0u;
    input->pot_button_y_seen[0] = 0u;
    input->pot_button_y_seen[1] = 0u;
}

uint16_t paula_input_read_joydat(PaulaInput *input, unsigned port)
{
    if (!input || port > 1u)
        return 0x0000u;

    return input->joydat[port];
}

uint16_t paula_input_read_potdat(PaulaInput *input, unsigned port)
{
    uint16_t value = 0xFFFFu;

    if (!input || port > 1u)
        return 0xFFFFu;

    if (port == 0u)
    {
        if (!(input->potgo & 0x0200u) && input->pot_button_x[0])
            value &= 0xFF00u;
        if (!(input->potgo & 0x0800u) && input->pot_button_y[0])
            value &= 0x00FFu;
        return value;
    }

    if (!(input->potgo & 0x2000u) && input->pot_button_x[1])
        value &= 0xFF00u;
    if (!(input->potgo & 0x8000u) && input->pot_button_y[1])
        value &= 0x00FFu;

    return value;
}

static void paula_input_log_pot_buttons_consumed(PaulaInput *input, uint16_t potgor)
{
#ifdef BELLATRIX_HARNESS
    if (input->pot_button_x[0] && !input->pot_button_x_seen[0] &&
        (input->potgo & 0x0200u) && (input->potgo & 0x0100u) &&
        !(potgor & 0x0100u)) {
        input->pot_button_x_seen[0] = 1u;
        kprintf("[MOUSE-MMB] pc=%08x port=0 read POTGOR=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)potgor);
    }

    if (input->pot_button_y[0] && !input->pot_button_y_seen[0] &&
        (input->potgo & 0x0800u) && (input->potgo & 0x0400u) &&
        !(potgor & 0x0400u)) {
        input->pot_button_y_seen[0] = 1u;
        kprintf("[MOUSE-RMB] pc=%08x port=0 read POTGOR=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)potgor);
    }

    if (input->pot_button_x[1] && !input->pot_button_x_seen[1] &&
        (input->potgo & 0x2000u) && (input->potgo & 0x1000u) &&
        !(potgor & 0x1000u)) {
        input->pot_button_x_seen[1] = 1u;
        kprintf("[MOUSE-MMB] pc=%08x port=1 read POTGOR=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)potgor);
    }

    if (input->pot_button_y[1] && !input->pot_button_y_seen[1] &&
        (input->potgo & 0x8000u) && (input->potgo & 0x4000u) &&
        !(potgor & 0x4000u)) {
        input->pot_button_y_seen[1] = 1u;
        kprintf("[MOUSE-RMB] pc=%08x port=1 read POTGOR=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)potgor);
    }
#else
    (void)input;
    (void)potgor;
#endif
}

uint16_t paula_input_read_potgor(PaulaInput *input)
{
    /* Low byte (bits 7-0): POT counter idle bits — always 0 when not
     * running a POT charge cycle.  Returning 0xFF here causes DiagROM's
     * GetMouseData to set DISPAULA=1, blocking all serial/mouse input. */
    uint16_t value = 0xFF00u;

    if ((input->potgo & 0x0200u) && (input->potgo & 0x0100u) &&
        input->pot_button_x[0]) {
        value &= (uint16_t)~0x0100u;
    }

    if ((input->potgo & 0x0800u) && (input->potgo & 0x0400u) &&
        input->pot_button_y[0]) {
        value &= (uint16_t)~0x0400u;
    }

    if ((input->potgo & 0x2000u) && (input->potgo & 0x1000u) &&
        input->pot_button_x[1]) {
        value &= (uint16_t)~0x1000u;
    }

    if ((input->potgo & 0x8000u) && (input->potgo & 0x4000u) &&
        input->pot_button_y[1]) {
        value &= (uint16_t)~0x4000u;
    }

    paula_input_log_pot_buttons_consumed(input, value);
    return value;
}

void paula_input_write_potgo(PaulaInput *input, uint16_t value)
{
    input->potgo = value;
}

void paula_input_set_joydat(PaulaInput *input, unsigned port, uint16_t value)
{
    if (!input || port > 1u)
        return;

    input->joydat[port] = value;
}

void paula_input_set_pot_button_x(PaulaInput *input, unsigned port, int pressed)
{
    if (port > 1u) {
        return;
    }

    input->pot_button_x[port] = pressed ? 1u : 0u;
    if (!pressed) {
        input->pot_button_x_seen[port] = 0u;
    }
}

void paula_input_set_pot_button_y(PaulaInput *input, unsigned port, int pressed)
{
    if (port > 1u) {
        return;
    }

    input->pot_button_y[port] = pressed ? 1u : 0u;
    if (!pressed) {
        input->pot_button_y_seen[port] = 0u;
    }
}
