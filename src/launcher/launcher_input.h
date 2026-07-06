// src/launcher/launcher_input.h
// Raw USB HID key queue for pre-emulation launcher UI.
// Compiled only when BELLATRIX_LAUNCHER is defined.

#pragma once
#include <stdint.h>
#include <stdbool.h>

#define LAUNCHER_INPUT_CAP 32u

// HID usage codes the launcher cares about
#define LAUNCHER_KEY_UP      0x52u
#define LAUNCHER_KEY_DOWN    0x51u
#define LAUNCHER_KEY_ENTER   0x28u
#define LAUNCHER_KEY_ESC     0x29u
#define LAUNCHER_KEY_SPACE   0x2Cu
#define LAUNCHER_KEY_KPENTER 0x58u
#define LAUNCHER_KEY_DEL     0x4Cu  /* HID Delete (forward delete) */
#define LAUNCHER_KEY_BKSP    0x2Au  /* HID Backspace */

void launcher_input_init(void);
void launcher_input_set_active(bool active);
bool launcher_input_is_active(void);

// Called from USB HID callback (interrupt context on bare-metal)
void launcher_input_push(uint8_t usage);

// Returns 0 if queue is empty
uint8_t launcher_input_pop(void);
