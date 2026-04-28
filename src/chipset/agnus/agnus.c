// src/chipset/agnus/agnus.c

#include "agnus.h"
#include "chipset/denise/denise.h"
#include "chipset/paula/paula.h"
#include "debug/cpu_pc.h"
#include "host/pal.h"
#include "support.h"

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

#define CHIP_RAM_ADDR_MASK 0x001FFFFFu
#define CHIP_RAM_WORD_MASK 0x001FFFFEu

static inline void agnus_apply_setclr_15(uint16_t *dst, uint16_t raw, uint16_t writable_mask)
{
    uint16_t bits = (uint16_t)(raw & writable_mask);
    if (raw & 0x8000u)
        *dst |= bits;
    else
        *dst &= (uint16_t)~bits;
}

/* ECS Fat Agnus 8372A PAL — bits [14:8] of VPOSR */
#define AGNUS_CHIP_ID 0x20u

static inline void agnus_get_beam(const AgnusState *s,
                                  uint16_t *vposr_out,
                                  uint16_t *vhposr_out)
{
    uint16_t lof = s->beam.lof ? 0x8000u : 0u;
    uint16_t vpos8 = (uint16_t)((s->beam.vpos >> 8) & 0x01u);
    *vposr_out = (uint16_t)(lof | (AGNUS_CHIP_ID << 8) | vpos8);
    *vhposr_out = (uint16_t)(((s->beam.vpos & 0xFFu) << 8) | (s->beam.hpos & 0xFFu));
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void agnus_init(AgnusState *s)
{
    s->dmacon = 0;

    beam_init(&s->beam);
    bitplanes_init(&s->bitplanes);
    s->memory = NULL;

    s->diwstrt = 0x2C81u;
    s->diwstop = 0xF4C1u;
    s->ddfstrt = 0x0038u;
    s->ddfstop = 0x00D0u;

    for (int i = 0; i < 6; i++)
    {
        s->bplpth[i] = 0;
        s->bplptl[i] = 0;
    }

    s->bpl1mod = 0;
    s->bpl2mod = 0;

    blitter_init(&s->blitter);
    copper_init(&s->copper);

    s->denise = NULL;
    s->paula = NULL;
}

void agnus_reset(AgnusState *s)
{
    struct Denise *saved_denise = s->denise;
    struct Paula *saved_paula = s->paula;
    BellatrixMemory *saved_mem = s->memory;

    agnus_init(s);

    s->denise = saved_denise;
    s->paula = saved_paula;
    s->memory = saved_mem;
}

/* ---------------------------------------------------------------------------
 * Wiring
 * ------------------------------------------------------------------------- */

void agnus_attach_denise(AgnusState *s, struct Denise *d)
{
    s->denise = d;
}

void agnus_attach_paula(AgnusState *s, struct Paula *p)
{
    s->paula = p;
}

void agnus_attach_memory(AgnusState *s, BellatrixMemory *m)
{
    s->memory = m;
}

/* ---------------------------------------------------------------------------
 * IRQ forwarding — Paula owns INTREQ/INTENA
 * ------------------------------------------------------------------------- */

void agnus_intreq_set(AgnusState *s, uint16_t bits)
{
    if (s->paula)
    {
        paula_irq_raise(s->paula, bits);
        uint16_t pending = (uint16_t)(s->paula->intena & s->paula->intreq & 0x3FFFu);
        kprintf("[IRQSET] bits=%04x intena=%04x intreq=%04x pending=%04x\n",
                (unsigned)bits, (unsigned)s->paula->intena,
                (unsigned)s->paula->intreq, (unsigned)pending);
    }
}

void agnus_intreq_clear(AgnusState *s, uint16_t bits)
{
    if (s->paula)
        paula_irq_clear(s->paula, bits);
}

/* ---------------------------------------------------------------------------
 * Busy state
 * ------------------------------------------------------------------------- */

int agnus_blitter_busy(const AgnusState *s)
{
    if (!blitter_is_busy(&s->blitter))
        return 0;
    if (!(s->dmacon & DMAF_DMAEN))
        return 0;
    if (!(s->dmacon & DMAF_BLTEN))
        return 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Step
 * ------------------------------------------------------------------------- */

void agnus_step(AgnusState *s, uint64_t ticks)
{
    if (!ticks)
        return;

    /* Step subcomponents that consume elapsed time directly. */
    blitter_step(&s->blitter, s, ticks);

    int vbl_before = beam_is_in_vblank(&s->beam);
    beam_step(&s->beam, ticks);

    if ((s->dmacon & DMAF_DMAEN) && (s->dmacon & DMAF_COPEN))
        copper_step(&s->copper, s, 1);

    bitplanes_step(&s->bitplanes, s);
    if (bitplanes_line_ready(&s->bitplanes))
    {
        if (s->denise)
            denise_render_line(s->denise, s, &s->bitplanes);
        bitplanes_clear_line_ready(&s->bitplanes);
    }

    if (!vbl_before && beam_is_in_vblank(&s->beam))
    {
        {
            uint16_t _intena = s->paula ? s->paula->intena : 0u;
            uint16_t _intreq = s->paula ? s->paula->intreq : 0u;
            uint16_t _pend = (uint16_t)(_intena & _intreq & 0x3FFFu);
            uint32_t _pc = bellatrix_debug_cpu_pc();
            kprintf("[VBL-ENTER] frame=%u hpos=%u vpos=%u dmacon=0x%04x intena=0x%04x intreq=0x%04x pending=0x%04x m68k_pc=%08x\n",
                    (unsigned)s->beam.frame,
                    (unsigned)s->beam.hpos,
                    (unsigned)s->beam.vpos,
                    (unsigned)s->dmacon,
                    (unsigned)_intena,
                    (unsigned)_intreq,
                    (unsigned)_pend,
                    (unsigned)_pc);
        }

        if (s->memory)
        {
            kprintf("[VBL-VECS] chip[60]=%08x [64]=%08x [68]=%08x [6c]=%08x [70]=%08x [78]=%08x\n",
                    (unsigned)bellatrix_chip_read32(s->memory, 0x60u),
                    (unsigned)bellatrix_chip_read32(s->memory, 0x64u),
                    (unsigned)bellatrix_chip_read32(s->memory, 0x68u),
                    (unsigned)bellatrix_chip_read32(s->memory, 0x6cu),
                    (unsigned)bellatrix_chip_read32(s->memory, 0x70u),
                    (unsigned)bellatrix_chip_read32(s->memory, 0x78u));
        }

        agnus_intreq_set(s, PAULA_INT_VERTB);

        kprintf("[FRAME] pre-reload  bplcon0=%04x bpl1=%04x/%04x dmacon=%04x\n",
                s->denise ? s->denise->bplcon0 : 0,
                s->bplpth[0], s->bplptl[0], (unsigned)s->dmacon);

        if ((s->dmacon & DMAF_DMAEN) && (s->dmacon & DMAF_COPEN))
        {
            copper_vbl_reload(&s->copper);

            if (s->memory)
            {
                uint32_t _lc = s->copper.pc & CHIP_RAM_WORD_MASK;

                kprintf("[COPPER-DUMP] lc=%05x : "
                        "%04x %04x %04x %04x %04x %04x %04x %04x "
                        "%04x %04x %04x %04x\n",
                        (unsigned)_lc,
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x00u) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x02u) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x04u) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x06u) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x08u) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x0au) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x0cu) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x0eu) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x10u) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x12u) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x14u) & CHIP_RAM_WORD_MASK),
                        (unsigned)bellatrix_chip_read16(s->memory, (_lc + 0x16u) & CHIP_RAM_WORD_MASK));
            }
        }
        else
        {
            kprintf("[COPPER] vbl_reload skipped — COPEN off (dmacon=%04x)\n",
                    (unsigned)s->dmacon);
        }

        kprintf("[DENISE] frame=%u bpt0=0x%05x dmacon=0x%04x\n",
                (unsigned)s->beam.frame,
                (unsigned)((((uint32_t)(s->bplpth[0] & 0x1Fu)) << 16) |
                           ((uint32_t)(s->bplptl[0] & 0xFFFEu))),
                (unsigned)s->dmacon);

        PAL_Video_Flip();
    }
}

/* ---------------------------------------------------------------------------
 * Bus protocol
 * ------------------------------------------------------------------------- */

int agnus_handles_read(const AgnusState *s, uint32_t addr)
{
    (void)s;
    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;
    uint16_t reg = (uint16_t)(addr & 0x1FEu);
    if (reg == 0x001Cu || reg == 0x001Eu)
        return 0;
    if (reg >= 0x0100u && reg != AGNUS_BPL1MOD && reg != AGNUS_BPL2MOD)
        return 0;
    return 1;
}

int agnus_handles_write(const AgnusState *s, uint32_t addr)
{
    (void)s;
    if (addr < 0xDFF000u || addr > 0xDFF1FFu)
        return 0;
    uint16_t reg = (uint16_t)(addr & 0x1FEu);
    if (reg == 0x009Au || reg == 0x009Cu)
        return 0;
    if (reg >= 0x0100u && reg != AGNUS_BPL1MOD && reg != AGNUS_BPL2MOD)
        return 0;
    return 1;
}

uint32_t agnus_read(AgnusState *s, uint32_t addr, unsigned int size)
{
    (void)size;
    return agnus_read_reg(s, (uint16_t)(addr & 0x1FEu));
}

void agnus_write(AgnusState *s, uint32_t addr, uint32_t value, unsigned int size)
{
    agnus_write_reg(s, (uint16_t)(addr & 0x1FEu), value, (int)size);
}

/* ---------------------------------------------------------------------------
 * Low-level register API
 * ------------------------------------------------------------------------- */

uint32_t agnus_read_reg(AgnusState *s, uint16_t reg)
{
    switch (reg)
    {
    case AGNUS_DMACONR:
        return (uint32_t)((uint16_t)(s->dmacon & ~(AGNUS_DMACON_BBUSY | AGNUS_DMACON_BZERO)) |
                          (uint16_t)(blitter_is_busy(&s->blitter) ? AGNUS_DMACON_BBUSY : 0u) |
                          (uint16_t)(s->blitter.zero ? AGNUS_DMACON_BZERO : 0u));

    case AGNUS_VPOSR:
    {
        uint16_t v, h;
        agnus_get_beam(s, &v, &h);
        return v;
    }

    case AGNUS_VHPOSR:
    {
        uint16_t v, h;
        agnus_get_beam(s, &v, &h);
        return h;
    }

    case AGNUS_COP1LCH:
    case AGNUS_COP1LCL:
    case AGNUS_COP2LCH:
    case AGNUS_COP2LCL:
        return copper_read_reg(&s->copper, reg);

    default:
        if (agnus_is_blitter_reg(reg))
            return blitter_read_reg(&s->blitter, reg);
        return 0;
    }
}

void agnus_write_reg(AgnusState *s, uint16_t reg, uint32_t value, int size)
{
    (void)size;
    uint16_t raw = (uint16_t)value;

    if (reg >= AGNUS_BPL1PTH && reg <= AGNUS_BPL6PTL)
    {
        kprintf("[BPL-WRITE] reg=%04x value=%04x pc=%08x\n",
                reg, raw, bellatrix_debug_cpu_pc());
    }

    switch (reg)
    {
    case AGNUS_DMACON:
    {
        uint16_t old = s->dmacon;
        agnus_apply_setclr_15(&s->dmacon, raw, 0x7FFFu);
        kprintf("[DMACON-W] pc=%08x raw=%04x old=%04x new=%04x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)raw, (unsigned)old, (unsigned)s->dmacon);
        return;
    }

        /* ---------------------------------------------------------------------------
         * Copper pointer registers (IMPORTANT FIX)
         * ------------------------------------------------------------------------- */

    case COPPER_COP1LCH:
        s->copper.cop1lc = (s->copper.cop1lc & 0x0000FFFFu) | ((uint32_t)raw << 16);
        kprintf("[AGNUS] COP1LCH pc=%08x value=%04x cop1lc=0x%06x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)raw,
                (unsigned)(s->copper.cop1lc & CHIP_RAM_ADDR_MASK));
        return;

    case COPPER_COP1LCL:
        s->copper.cop1lc = (s->copper.cop1lc & 0xFFFF0000u) | (uint32_t)(raw & 0xFFFEu);
        kprintf("[AGNUS] COP1LCL pc=%08x value=%04x cop1lc=0x%06x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)raw,
                (unsigned)(s->copper.cop1lc & CHIP_RAM_ADDR_MASK));
        return;

    case COPPER_COP2LCH:
        s->copper.cop2lc = (s->copper.cop2lc & 0x0000FFFFu) | ((uint32_t)raw << 16);
        kprintf("[AGNUS] COP2LCH pc=%08x value=%04x cop2lc=0x%06x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)raw,
                (unsigned)(s->copper.cop2lc & CHIP_RAM_ADDR_MASK));
        return;

    case COPPER_COP2LCL:
        s->copper.cop2lc = (s->copper.cop2lc & 0xFFFF0000u) | (uint32_t)(raw & 0xFFFEu);
        kprintf("[AGNUS] COP2LCL pc=%08x value=%04x cop2lc=0x%06x\n",
                (unsigned)bellatrix_debug_cpu_pc(),
                (unsigned)raw,
                (unsigned)(s->copper.cop2lc & CHIP_RAM_ADDR_MASK));
        return;

    case AGNUS_DIWSTRT:
        s->diwstrt = raw;
        return;
    case AGNUS_DIWSTOP:
        s->diwstop = raw;
        return;
    case AGNUS_DDFSTRT:
        s->ddfstrt = raw;
        return;
    case AGNUS_DDFSTOP:
        s->ddfstop = raw;
        return;

    case AGNUS_BPL1PTH:
        s->bplpth[0] = raw;
        kprintf("[AGNUS] BPL1PTH=%04x ptr=%05x\n", (unsigned)raw,
                (unsigned)((((uint32_t)(raw & 0x1Fu)) << 16) | s->bplptl[0]));
        return;
    case AGNUS_BPL1PTL:
        s->bplptl[0] = raw;
        kprintf("[AGNUS] BPL1PTL=%04x ptr=%05x\n", (unsigned)raw,
                (unsigned)((((uint32_t)(s->bplpth[0] & 0x1Fu)) << 16) | (raw & 0xFFFEu)));
        return;
    case AGNUS_BPL2PTH:
        s->bplpth[1] = raw;
        kprintf("[AGNUS] BPL2PTH=%04x\n", (unsigned)raw);
        return;
    case AGNUS_BPL2PTL:
        s->bplptl[1] = raw;
        kprintf("[AGNUS] BPL2PTL=%04x\n", (unsigned)raw);
        return;
    case AGNUS_BPL3PTH:
        s->bplpth[2] = raw;
        kprintf("[AGNUS] BPL3PTH=%04x\n", (unsigned)raw);
        return;
    case AGNUS_BPL3PTL:
        s->bplptl[2] = raw;
        kprintf("[AGNUS] BPL3PTL=%04x\n", (unsigned)raw);
        return;
    case AGNUS_BPL4PTH:
        s->bplpth[3] = raw;
        kprintf("[AGNUS] BPL4PTH=%04x\n", (unsigned)raw);
        return;
    case AGNUS_BPL4PTL:
        s->bplptl[3] = raw;
        kprintf("[AGNUS] BPL4PTL=%04x\n", (unsigned)raw);
        return;
    case AGNUS_BPL5PTH:
        s->bplpth[4] = raw;
        kprintf("[AGNUS] BPL5PTH=%04x\n", (unsigned)raw);
        return;
    case AGNUS_BPL5PTL:
        s->bplptl[4] = raw;
        kprintf("[AGNUS] BPL5PTL=%04x\n", (unsigned)raw);
        return;
    case AGNUS_BPL6PTH:
        s->bplpth[5] = raw;
        kprintf("[AGNUS] BPL6PTH=%04x\n", (unsigned)raw);
        return;
    case AGNUS_BPL6PTL:
        s->bplptl[5] = raw;
        kprintf("[AGNUS] BPL6PTL=%04x\n", (unsigned)raw);
        return;

    case AGNUS_BPL1MOD:
        s->bpl1mod = (int16_t)raw;
        return;
    case AGNUS_BPL2MOD:
        s->bpl2mod = (int16_t)raw;
        return;

    default:
        break;
    }

    if (reg == 0x009Au || reg == 0x009Cu)
    {
        if (s->paula)
            paula_write(s->paula, 0xDFF000u | reg, raw, 2);
        return;
    }

    if (agnus_is_blitter_reg(reg))
    {
        blitter_write_reg(&s->blitter, s, reg, raw);
        return;
    }

    /*
     * COPJMP1/COPJMP2/COPINS remain Copper-side commands.
     * COP1LC/COP2LC are handled above as Agnus-owned pointer registers.
     */
    if (agnus_is_copper_reg(reg))
    {
        copper_write_reg(&s->copper, reg, raw);
        return;
    }

    if (reg >= 0x0100u &&
        reg != AGNUS_BPL1MOD && reg != AGNUS_BPL2MOD)
    {
        if (reg == AGNUS_BPLCON0)
            s->bplcon0 = raw;
        if (s->denise)
            denise_write_reg(s->denise, reg, raw);
        return;
    }

    (void)raw;
}
