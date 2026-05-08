#ifndef BELLATRIX_PAULA_INPUT_H
#define BELLATRIX_PAULA_INPUT_H

#include <stdint.h>

typedef struct PaulaInput
{
    uint16_t potgo;
    uint16_t joydat[2];
    uint8_t pot_button_x[2];
    uint8_t pot_button_y[2];
    uint8_t pot_button_x_seen[2];
    uint8_t pot_button_y_seen[2];
} PaulaInput;

void paula_input_init(PaulaInput *input);
void paula_input_reset(PaulaInput *input);

uint16_t paula_input_read_joydat(PaulaInput *input, unsigned port);
uint16_t paula_input_read_potdat(PaulaInput *input, unsigned port);
uint16_t paula_input_read_potgor(PaulaInput *input);
void paula_input_write_potgo(PaulaInput *input, uint16_t value);
void paula_input_set_joydat(PaulaInput *input, unsigned port, uint16_t value);
void paula_input_set_pot_button_x(PaulaInput *input, unsigned port, int pressed);
void paula_input_set_pot_button_y(PaulaInput *input, unsigned port, int pressed);

#endif
