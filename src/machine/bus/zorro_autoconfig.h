#ifndef BELLATRIX_BUS_ZORRO_AUTOCONFIG_H
#define BELLATRIX_BUS_ZORRO_AUTOCONFIG_H

#include <stdint.h>

/*
 * The $E80000 AutoConfig window predicate. AutoConfig accesses in this window
 * are answered by the board_registry walker (see machine/bus/board_registry.h);
 * this header only classifies the address range.
 */
int bellatrix_zorro_autoconfig_in_window(uint32_t addr);

#endif
