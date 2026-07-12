#include "cpu/mmio_policy.h"

BellatrixMmioPolicy bellatrix_mmio_policy(uint32_t addr, unsigned int size,
                                          int is_write)
{
    uint32_t reg;

    if (size != 1u && size != 2u && size != 4u)
        return BELLATRIX_MMIO_DIRECT;
    if (addr >= 0x00BFD000u && addr <= 0x00BFEFFFu)
        return BELLATRIX_MMIO_SYNC;
    if (addr < 0x00DFF000u || addr > 0x00DFF1FFu)
        return BELLATRIX_MMIO_DIRECT;

    reg = addr & 0x1ffu;
    if (is_write) {
        switch (reg) {
        case 0x024u: /* DSKLEN */
        case 0x02au: /* VPOSW */
        case 0x02cu: /* VHPOSW */
        case 0x058u: /* BLTSIZE */
        case 0x05cu: /* BLTSIZV */
        case 0x05eu: /* BLTSIZH */
        case 0x088u: /* COPJMP1 */
        case 0x08au: /* COPJMP2 */
        case 0x092u: /* DDFSTRT */
        case 0x094u: /* DDFSTOP */
        case 0x096u: /* DMACON */
        case 0x09au: /* INTENA */
        case 0x09cu: /* INTREQ */
        case 0x1dcu: /* BEAMCON0 */
            return BELLATRIX_MMIO_SYNC;
        default:
            return BELLATRIX_MMIO_POSTED;
        }
    }

    switch (reg) {
    case 0x002u: /* DMACONR */
    case 0x004u: /* VPOSR */
    case 0x006u: /* VHPOSR */
    case 0x01cu: /* INTENAR */
    case 0x01eu: /* INTREQR */
        return BELLATRIX_MMIO_PUBLISHED;
    case 0x008u: /* DSKDATR */
    case 0x010u: /* ADKCONR */
    case 0x016u: /* POTGOR/POTINP */
    case 0x018u: /* SERDATR */
    case 0x01au: /* DSKBYTR */
        return BELLATRIX_MMIO_SYNC;
    default:
        return BELLATRIX_MMIO_DIRECT;
    }
}
