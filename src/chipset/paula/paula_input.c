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
    input->mouse_right[0] = 0u;
    input->mouse_right[1] = 0u;
    input->mouse_right_seen[0] = 0u;
    input->mouse_right_seen[1] = 0u;
}

static void paula_input_log_mouse_right_consumed(PaulaInput *input, uint16_t potgor)
{
#ifdef BELLATRIX_HARNESS
    if (input->mouse_right[0] && !input->mouse_right_seen[0] &&
        (input->potgo & 0x0800u) && (input->potgo & 0x0400u) &&
        !(potgor & 0x0400u)) {
        input->mouse_right_seen[0] = 1u;
        kprintf("[MOUSE-RMB] pc=%08x port=0 read POTGOR=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)potgor);
    }

    if (input->mouse_right[1] && !input->mouse_right_seen[1] &&
        (input->potgo & 0x8000u) && (input->potgo & 0x4000u) &&
        !(potgor & 0x4000u)) {
        input->mouse_right_seen[1] = 1u;
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
    uint16_t value = 0xFFFFu;

    if ((input->potgo & 0x0800u) && (input->potgo & 0x0400u) &&
        input->mouse_right[0]) {
        value &= (uint16_t)~0x0400u;
    }

    if ((input->potgo & 0x8000u) && (input->potgo & 0x4000u) &&
        input->mouse_right[1]) {
        value &= (uint16_t)~0x4000u;
    }

    paula_input_log_mouse_right_consumed(input, value);
    return value;
}

void paula_input_write_potgo(PaulaInput *input, uint16_t value)
{
    input->potgo = value;
}

void paula_input_set_mouse_right(PaulaInput *input, unsigned port, int pressed)
{
    if (port > 1u) {
        return;
    }

    input->mouse_right[port] = pressed ? 1u : 0u;
    if (!pressed) {
        input->mouse_right_seen[port] = 0u;
    }
}
