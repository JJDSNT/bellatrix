#ifndef BELLATRIX_CIA_SERIAL_H
#define BELLATRIX_CIA_SERIAL_H

#include <stdint.h>

typedef struct CIA_State CIA;

void    cia_serial_reset(CIA *cia);
void    cia_serial_set_sp_input(CIA *cia, uint8_t level);
void    cia_serial_on_cnt_rising(CIA *cia);
void    cia_serial_on_timer_a_underflow(CIA *cia);
void    cia_serial_write_sdr(CIA *cia, uint8_t value);
int     cia_serial_receive_byte(CIA *cia, uint8_t value);
uint8_t cia_serial_sp_output_level(const CIA *cia);
uint8_t cia_serial_cnt_output_level(const CIA *cia);

#endif
