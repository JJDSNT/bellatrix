#ifndef BELLATRIX_CIA_TIMERS_H
#define BELLATRIX_CIA_TIMERS_H

#include <stdint.h>

typedef struct CIA_State CIA;

void    cia_timers_reset(CIA *cia);
void    cia_timers_step(CIA *cia, uint64_t ticks);
void    cia_timers_count_cnt(CIA *cia, uint32_t pulses);
void    cia_timers_advance_pb_outputs(CIA *cia);
int     cia_timers_read_reg(CIA *cia, uint8_t reg, uint8_t *value);
int     cia_timers_write_reg(CIA *cia, uint8_t reg, uint8_t value);

#endif
