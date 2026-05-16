// src/launcher/launcher_input.c

#include "launcher/launcher_input.h"
#include <string.h>

static bool    s_active;
static uint8_t s_buf[LAUNCHER_INPUT_CAP];
static uint8_t s_head;
static uint8_t s_tail;
static uint8_t s_count;

void launcher_input_init(void)
{
    s_active = false;
    s_head   = 0;
    s_tail   = 0;
    s_count  = 0;
}

void launcher_input_set_active(bool active)
{
    s_active = active;
}

bool launcher_input_is_active(void)
{
    return s_active;
}

void launcher_input_push(uint8_t usage)
{
    if (s_count >= LAUNCHER_INPUT_CAP) {
        return;
    }
    s_buf[s_tail] = usage;
    s_tail = (uint8_t)((s_tail + 1u) % LAUNCHER_INPUT_CAP);
    s_count++;
}

uint8_t launcher_input_pop(void)
{
    if (s_count == 0u) {
        return 0u;
    }
    uint8_t v = s_buf[s_head];
    s_head = (uint8_t)((s_head + 1u) % LAUNCHER_INPUT_CAP);
    s_count--;
    return v;
}
