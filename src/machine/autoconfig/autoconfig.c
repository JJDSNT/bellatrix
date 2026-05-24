#include "machine/autoconfig/autoconfig.h"

#include <string.h>

void autoconfig_build(uint8_t *out, const uint8_t *bytes)
{
    unsigned int i;

    memset(out, 0x00u, AUTOCONFIG_DATA_SIZE);

    for (i = 0; i < AUTOCONFIG_ROM_BYTES; i++) {
        /* AROS ReadExpansionRom does: field = ~ReadExpansionByte(N), then
         * double-inverts er_Type (index 0).  Store complement here so AROS
         * recovers the original value.  er_Type is left as-is because the
         * two inversions cancel out. */
        uint8_t b = (i == 0) ? bytes[i] : (uint8_t)(~bytes[i]);
        out[i * 4u]      = (uint8_t)(b & 0xF0u);
        out[i * 4u + 2u] = (uint8_t)((b & 0x0Fu) << 4);
    }
}
