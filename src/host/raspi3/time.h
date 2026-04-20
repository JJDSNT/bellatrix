// src/host/raspi3/time.h
#ifndef BELLATRIX_HOST_RASPI3_TIME_H
#define BELLATRIX_HOST_RASPI3_TIME_H

#include <stdint.h>

uint64_t raspi3_counter_get(void);
uint64_t raspi3_counter_freq(void);
void raspi3_delay_us(uint64_t delta_us);
void raspi3_delay_ticks(uint64_t ticks);
void raspi3_delay_ticks_wfe(uint64_t ticks);
void raspi3_wait_for_event(void);

#endif