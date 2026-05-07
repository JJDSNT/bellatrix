#ifndef BELLATRIX_PAULA_INPUT_H
#define BELLATRIX_PAULA_INPUT_H

#include <stdint.h>

typedef struct PaulaInput
{
    uint16_t potgo;
    uint8_t mouse_right[2];
    uint8_t mouse_right_seen[2];
} PaulaInput;

void paula_input_init(PaulaInput *input);
void paula_input_reset(PaulaInput *input);

uint16_t paula_input_read_potgor(PaulaInput *input);
void paula_input_write_potgo(PaulaInput *input, uint16_t value);
void paula_input_set_mouse_right(PaulaInput *input, unsigned port, int pressed);

#endif
