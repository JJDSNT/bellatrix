#include "machine/input/controller_port.h"

#include <string.h>

static BellatrixControllerPortState *controller_port_state(BellatrixControllerPorts *ports,
                                                           unsigned port)
{
    if (!ports || port > 1u)
        return NULL;

    return &ports->port[port];
}

static const BellatrixControllerPortState *controller_port_state_const(const BellatrixControllerPorts *ports,
                                                                       unsigned port)
{
    if (!ports || port > 1u)
        return NULL;

    return &ports->port[port];
}

void bellatrix_controller_ports_init(BellatrixControllerPorts *ports)
{
    memset(ports, 0, sizeof(*ports));
    bellatrix_controller_ports_reset(ports);
}

void bellatrix_controller_ports_reset(BellatrixControllerPorts *ports)
{
    if (!ports)
        return;

    memset(ports, 0, sizeof(*ports));
    ports->port[0].device = BELLATRIX_CONTROLLER_PORT_MOUSE;
    ports->port[1].device = BELLATRIX_CONTROLLER_PORT_MOUSE;
}

void bellatrix_controller_port_set_device(BellatrixControllerPorts *ports,
                                          unsigned port,
                                          BellatrixControllerPortDevice device)
{
    BellatrixControllerPortState *state = controller_port_state(ports, port);

    if (!state)
        return;

    state->device = (uint8_t)device;
}

void bellatrix_controller_port_set_mouse_button(BellatrixControllerPorts *ports,
                                                unsigned port,
                                                unsigned button,
                                                int pressed)
{
    BellatrixControllerPortState *state = controller_port_state(ports, port);
    uint8_t value = pressed ? 1u : 0u;

    if (!state)
        return;

    state->device = BELLATRIX_CONTROLLER_PORT_MOUSE;

    switch (button)
    {
    case BELLATRIX_MOUSE_BUTTON_LEFT:
        state->fire = value;
        return;
    case BELLATRIX_MOUSE_BUTTON_MIDDLE:
        state->button2 = value;
        return;
    case BELLATRIX_MOUSE_BUTTON_RIGHT:
        state->button3 = value;
        return;
    default:
        return;
    }
}

void bellatrix_controller_port_add_mouse_motion(BellatrixControllerPorts *ports,
                                                unsigned port,
                                                int dx,
                                                int dy)
{
    BellatrixControllerPortState *state = controller_port_state(ports, port);

    if (!state)
        return;

    state->device = BELLATRIX_CONTROLLER_PORT_MOUSE;
    state->mouse_x = (uint8_t)(state->mouse_x + dx);
    state->mouse_y = (uint8_t)(state->mouse_y + dy);
}

void bellatrix_controller_port_set_joystick_button(BellatrixControllerPorts *ports,
                                                   unsigned port,
                                                   unsigned button,
                                                   int pressed)
{
    BellatrixControllerPortState *state = controller_port_state(ports, port);
    uint8_t value = pressed ? 1u : 0u;

    if (!state)
        return;

    state->device = BELLATRIX_CONTROLLER_PORT_JOYSTICK;

    switch (button)
    {
    case BELLATRIX_CONTROLLER_BUTTON_PRIMARY:
        state->fire = value;
        return;
    case BELLATRIX_CONTROLLER_BUTTON_SECONDARY:
        state->button2 = value;
        return;
    case BELLATRIX_CONTROLLER_BUTTON_TERTIARY:
        state->button3 = value;
        return;
    default:
        return;
    }
}

void bellatrix_controller_port_set_joystick_direction(BellatrixControllerPorts *ports,
                                                      unsigned port,
                                                      unsigned direction,
                                                      int pressed)
{
    BellatrixControllerPortState *state = controller_port_state(ports, port);
    uint8_t value = pressed ? 1u : 0u;

    if (!state)
        return;

    state->device = BELLATRIX_CONTROLLER_PORT_JOYSTICK;

    switch (direction)
    {
    case BELLATRIX_JOYSTICK_UP:
        state->joy_up = value;
        return;
    case BELLATRIX_JOYSTICK_DOWN:
        state->joy_down = value;
        return;
    case BELLATRIX_JOYSTICK_LEFT:
        state->joy_left = value;
        return;
    case BELLATRIX_JOYSTICK_RIGHT:
        state->joy_right = value;
        return;
    default:
        return;
    }
}

uint8_t bellatrix_controller_ports_cia_pra_bits(const BellatrixControllerPorts *ports)
{
    uint8_t value = 0xC0u;
    const BellatrixControllerPortState *port0 = controller_port_state_const(ports, 0u);
    const BellatrixControllerPortState *port1 = controller_port_state_const(ports, 1u);

    if (port0 && port0->fire)
        value &= (uint8_t)~0x40u;
    if (port1 && port1->fire)
        value &= (uint8_t)~0x80u;

    return value;
}

