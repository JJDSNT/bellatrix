#include "debug/emu_debug.h"

#include <stdint.h>

#include "core/machine.h"
#include "support.h"

/* ------------------------------------------------------------------------- */
/* RAM access                                                                */
/* ------------------------------------------------------------------------- */

static uint8_t ram8(BellatrixMachine *m, uint32_t a)
{
    if (!m || !m->memory.chip_ram || a >= m->memory.chip_ram_size) {
        return 0xFF;
    }
    return m->memory.chip_ram[a];
}

static uint16_t be16(BellatrixMachine *m, uint32_t a)
{
    return ((uint16_t)ram8(m, a) << 8) | ram8(m, a + 1);
}

/* ------------------------------------------------------------------------- */

static const char *dma_bit_name(int bit)
{
    switch (bit) {
        case 9: return "DMAEN";
        case 8: return "BPLEN";
        case 7: return "COPEN";
        case 6: return "BLTEN";
        case 5: return "SPREN";
        case 4: return "DSKEN";
        case 3: return "AUD3E";
        case 2: return "AUD2E";
        case 1: return "AUD1E";
        case 0: return "AUD0E";
        default: return 0;
    }
}

void emu_debug_dma(BellatrixMachine *m)
{
    uint16_t dmaconr;
    int bit;

    if (!m) {
        return;
    }

    kprintf("[DMA] ---- DMA state ----\n");

    dmaconr = m->agnus.dmacon;

    kprintf("[DMA] DMACONR=0x%04x enabled:", dmaconr);
    for (bit = 9; bit >= 0; bit--) {
        if (dmaconr & (1u << bit)) {
            const char *n = dma_bit_name(bit);
            if (n) {
                kprintf(" %s", n);
            }
        }
    }
    kprintf("\n");

    kprintf("[DMA] Beam V=%04x H=%04x\n",
            (unsigned)m->agnus.beam.vpos,
            (unsigned)m->agnus.beam.hpos);

    {
        int i;
        for (i = 0; i < 6; i++) {
            uint32_t pt = ((uint32_t)m->agnus.bplpth[i] << 16) | m->agnus.bplptl[i];
            kprintf("[DMA] BPL%dPT=%08x\n", i + 1, pt);
        }
    }

    kprintf("[DMA] ---- end ----\n");
}

static const char *cop_reg_name(uint16_t offset)
{
    switch (offset) {
        case 0x080: return "COP1LCH";
        case 0x082: return "COP1LCL";
        case 0x084: return "COP2LCH";
        case 0x086: return "COP2LCL";
        case 0x088: return "COPJMP1";
        case 0x08A: return "COPJMP2";
        case 0x08E: return "DIWSTRT";
        case 0x090: return "DIWSTOP";
        case 0x092: return "DDFSTRT";
        case 0x094: return "DDFSTOP";
        case 0x096: return "DMACON";
        case 0x09A: return "INTENA";
        case 0x09C: return "INTREQ";
        case 0x09E: return "ADKCON";
        case 0x100: return "BPLCON0";
        case 0x102: return "BPLCON1";
        case 0x104: return "BPLCON2";
        case 0x108: return "BPL1MOD";
        case 0x10A: return "BPL2MOD";
        case 0x0E0: return "BPL1PTH";
        case 0x0E2: return "BPL1PTL";
        case 0x0E4: return "BPL2PTH";
        case 0x0E6: return "BPL2PTL";
        case 0x0E8: return "BPL3PTH";
        case 0x0EA: return "BPL3PTL";
        case 0x0EC: return "BPL4PTH";
        case 0x0EE: return "BPL4PTL";
        case 0x0F0: return "BPL5PTH";
        case 0x0F2: return "BPL5PTL";
        case 0x0F4: return "BPL6PTH";
        case 0x0F6: return "BPL6PTL";
        case 0x180: return "COLOR00";
        case 0x182: return "COLOR01";
        case 0x184: return "COLOR02";
        case 0x186: return "COLOR03";
        case 0x188: return "COLOR04";
        case 0x18A: return "COLOR05";
        case 0x18C: return "COLOR06";
        case 0x18E: return "COLOR07";
        default: return 0;
    }
}

void emu_debug_copper(BellatrixMachine *m, uint32_t max_insn)
{
    uint32_t pc;
    uint32_t i;

    if (!m) {
        return;
    }

    kprintf("[COP] ---- Copper state ----\n");

    kprintf("[COP] state=%u PC=%08x cop1lc=%08x cop2lc=%08x\n",
            (unsigned)m->agnus.copper.state,
            (unsigned)m->agnus.copper.pc,
            (unsigned)m->agnus.copper.cop1lc,
            (unsigned)m->agnus.copper.cop2lc);

    pc = m->agnus.copper.pc;

    for (i = 0; i < max_insn; i++, pc += 4) {
        uint16_t ir1;
        uint16_t ir2;

        if (pc + 3 >= m->memory.chip_ram_size) {
            break;
        }

        ir1 = be16(m, pc);
        ir2 = be16(m, pc + 2);

        if (ir1 & 1) {
            kprintf("[COP] %08x: %s V=%02x H=%02x VM=%02x HM=%02x\n",
                    (unsigned)pc,
                    (ir2 & 1) ? "SKIP" : "WAIT",
                    (unsigned)(ir1 >> 8),
                    (unsigned)(ir1 & 0xFE),
                    (unsigned)(ir2 >> 8),
                    (unsigned)(ir2 & 0xFE));

            if (ir1 == 0xFFFF && ir2 == 0xFFFE) {
                kprintf("[COP] (end of list)\n");
                break;
            }
        } else {
            uint16_t reg = ir1 & 0x1FE;
            const char *name = cop_reg_name(reg);

            if (name) {
                kprintf("[COP] %08x: MOVE %04x -> %s\n",
                        (unsigned)pc, ir2, name);
            } else {
                kprintf("[COP] %08x: MOVE %04x -> %03x\n",
                        (unsigned)pc, ir2, reg);
            }
        }
    }

    kprintf("[COP] ---- end ----\n");
}

void emu_debug_mem(BellatrixMachine *m, uint32_t addr, uint32_t len)
{
    uint32_t off;

    if (!m) {
        return;
    }

    kprintf("[MEM] ---- memory dump ----\n");

    for (off = 0; off < len; off += 16) {
        uint32_t col;

        kprintf("%08x  ", (unsigned)(addr + off));

        for (col = 0; col < 16 && off + col < len; col++) {
            kprintf("%02x ", ram8(m, addr + off + col));
            if (col == 7) {
                kprintf(" ");
            }
        }

        kprintf(" |");

        for (col = 0; col < 16 && off + col < len; col++) {
            uint8_t v = ram8(m, addr + off + col);
            kprintf("%c", (v >= 32 && v < 127) ? v : '.');
        }

        kprintf("|\n");
    }

    kprintf("[MEM] ---- end ----\n");
}