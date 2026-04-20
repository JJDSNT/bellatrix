#include <stdint.h>
#include "cpu_iface.h"
#include "M68k.h"

/*
 * Interface da CPU ligada diretamente ao estado real do Emu68.
 */

extern struct M68KState *__m68k_state;

void cpu_set_fc(uint8_t fc)
{
    __m68k_state->FC = fc;
}

uint8_t cpu_get_fc(void)
{
    return __m68k_state->FC;
}