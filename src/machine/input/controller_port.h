#ifndef BELLATRIX_INPUT_CONTROLLER_PORT_H
#define BELLATRIX_INPUT_CONTROLLER_PORT_H

#include <stdint.h>

typedef enum BellatrixControllerPortDevice {
    BELLATRIX_CONTROLLER_PORT_MOUSE = 0,
    BELLATRIX_CONTROLLER_PORT_JOYSTICK = 1,
} BellatrixControllerPortDevice;

typedef enum BellatrixControllerButton {
    BELLATRIX_CONTROLLER_BUTTON_PRIMARY = 0,
    BELLATRIX_CONTROLLER_BUTTON_SECONDARY = 1,
    BELLATRIX_CONTROLLER_BUTTON_TERTIARY = 2,
} BellatrixControllerButton;

enum {
    BELLATRIX_MOUSE_BUTTON_LEFT = 0,
    BELLATRIX_MOUSE_BUTTON_MIDDLE = 1,
    BELLATRIX_MOUSE_BUTTON_RIGHT = 2,
};

typedef enum BellatrixJoystickDirection {
    BELLATRIX_JOYSTICK_UP = 0,
    BELLATRIX_JOYSTICK_DOWN = 1,
    BELLATRIX_JOYSTICK_LEFT = 2,
    BELLATRIX_JOYSTICK_RIGHT = 3,
} BellatrixJoystickDirection;

typedef struct BellatrixControllerPortState {
    uint8_t device;
    uint8_t fire;
    uint8_t button2;
    uint8_t button3;
    uint8_t mouse_x;
    uint8_t mouse_y;
    uint8_t joy_up;
    uint8_t joy_down;
    uint8_t joy_left;
    uint8_t joy_right;
} BellatrixControllerPortState;

typedef struct BellatrixControllerPorts {
    BellatrixControllerPortState port[2];
} BellatrixControllerPorts;

void bellatrix_controller_ports_init(BellatrixControllerPorts *ports);
void bellatrix_controller_ports_reset(BellatrixControllerPorts *ports);

void bellatrix_controller_port_set_device(BellatrixControllerPorts *ports,
                                          unsigned port,
                                          BellatrixControllerPortDevice device);
void bellatrix_controller_port_set_mouse_button(BellatrixControllerPorts *ports,
                                                unsigned port,
                                                unsigned button,
                                                int pressed);
void bellatrix_controller_port_add_mouse_motion(BellatrixControllerPorts *ports,
                                                unsigned port,
                                                int dx,
                                                int dy);
void bellatrix_controller_port_set_joystick_button(BellatrixControllerPorts *ports,
                                                   unsigned port,
                                                   unsigned button,
                                                   int pressed);
void bellatrix_controller_port_set_joystick_direction(BellatrixControllerPorts *ports,
                                                      unsigned port,
                                                      unsigned direction,
                                                      int pressed);

uint8_t bellatrix_controller_ports_cia_pra_bits(const BellatrixControllerPorts *ports);

#endif
