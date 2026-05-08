#ifndef BELLATRIX_CIA_PORTS_H
#define BELLATRIX_CIA_PORTS_H

#include <stdint.h>

typedef struct CIA_State CIA;

void cia_ports_reset(CIA *cia);
void cia_ports_apply_defaults(CIA *cia);

uint8_t cia_port_a_value(const CIA *cia);
uint8_t cia_port_b_value(const CIA *cia);

void cia_set_external_pra(CIA *cia, uint8_t value);
void cia_set_external_prb(CIA *cia, uint8_t value);
void cia_set_sp_level(CIA *cia, uint8_t level);
void cia_set_cnt_level(CIA *cia, uint8_t level);
void cia_set_flag_level(CIA *cia, uint8_t level);
void cia_pulse_cnt(CIA *cia, uint32_t pulses);
int  cia_ports_read_reg(CIA *cia, uint8_t reg, uint8_t *value);
int  cia_ports_write_reg(CIA *cia, uint8_t reg, uint8_t value);

#endif
