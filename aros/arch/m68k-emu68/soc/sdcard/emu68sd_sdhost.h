#ifndef _SDHOST_H
#define _SDHOST_H

#include "sdcard.h"

ULONG sdhost_getclock(struct SDCardBase *SDCardBase);
int sdhost_powerCycle(struct SDCardBase *SDCardBase);
void sdhost_led(int on, struct SDCardBase *SDCardBase);
void sdhost_led_inverted(int on, struct SDCardBase *SDCardBase);
void sdhost_cmd_int(ULONG cmd, ULONG arg, ULONG timeout, struct SDCardBase *SDCardBase);
void sdhost_cmd(ULONG command, ULONG arg, ULONG timeout, struct SDCardBase *SDCardBase);
int sdhost_card_init(struct SDCardBase *SDCardBase);
int sdhost_write(uint8_t *buf, uint32_t buf_size, uint32_t block_no, struct SDCardBase *SDCardBase);
int sdhost_read(uint8_t *buf, uint32_t buf_size, uint32_t block_no, struct SDCardBase *SDCardBase);
int sdhost_irq();
int sdhost_irq_gate();

#endif /* _SDHOST_H */