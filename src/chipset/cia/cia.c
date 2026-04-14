// src/chipset/cia/cia.c
//
// MOS 8520 CIA emulation.
//
// Timers: lazy real-time update using CNTPCT_EL0.
//   On every register access, elapsed ARM counter ticks are converted
//   to CIA ticks (@ 709379 Hz PAL) and the counter is advanced.
//   On underflow the ICR flag is set; continuous timers reload from latch.
//
// ICR: read-clear — reading the register atomically snapshots and clears it.
//
// TOD: increments at VBL rate (50 Hz). Phase 2: uses real time via CNTPCT.
//   Phase 3 replaces with explicit cia_vbl_tick() from the chipset loop.

#include "cia.h"
#include <stddef.h>

// ---------------------------------------------------------------------------
// ARM counter helpers
// ---------------------------------------------------------------------------

static uint64_t read_cntpct(void)
{
    uint64_t v;
    asm volatile("mrs %0, CNTPCT_EL0" : "=r"(v));
    return v;
}

static uint64_t read_cntfrq(void)
{
    uint64_t v;
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(v));
    return v ? v : 19200000ULL; // default 19.2 MHz if register returns 0
}

// Cache the ARM timer frequency at first use.
static uint64_t s_cntfrq = 0;

static uint64_t cntfrq(void)
{
    if (!s_cntfrq)
        s_cntfrq = read_cntfrq();
    return s_cntfrq;
}

// Convert ARM ticks to CIA ticks (PAL, 709379 Hz).
// Returns CIA ticks elapsed; safe for large values.
static uint64_t arm_to_cia_ticks(uint64_t arm_ticks)
{
    return (arm_ticks * CIA_FREQ_PAL_HZ) / cntfrq();
}

// ARM ticks per TOD tick (50 Hz VBL = 20 ms).
#define TOD_HZ 50ULL
static uint64_t arm_per_tod_tick(void)
{
    return cntfrq() / TOD_HZ;
}

// ---------------------------------------------------------------------------
// Internal: advance a single timer, return number of underflows
// ---------------------------------------------------------------------------

static int timer_advance(uint16_t *counter, uint16_t latch,
                         bool continuous, uint64_t cia_ticks)
{
    if (cia_ticks == 0) return 0;

    int underflows = 0;

    if (cia_ticks > *counter) {
        underflows = 1;
        cia_ticks -= *counter + 1; // ticks remaining after first underflow

        if (continuous) {
            if (latch > 0) {
                // Additional full underflows from latch
                underflows += (int)(cia_ticks / (latch + 1));
                *counter = latch - (uint16_t)(cia_ticks % (latch + 1));
            } else {
                *counter = 0;
            }
        } else {
            // One-shot: stays at 0 after underflow
            *counter = 0;
        }
    } else {
        *counter -= (uint16_t)cia_ticks;
    }

    return underflows;
}

// ---------------------------------------------------------------------------
// Update Timer A based on elapsed real time
// ---------------------------------------------------------------------------

static void update_ta(CIA_State *cia)
{
    if (!(cia->cra & CIA_CRA_START))
        return; // stopped
    if (cia->cra & CIA_CRA_INMODE)
        return; // CNT pin mode — not implemented

    uint64_t now = read_cntpct();
    uint64_t arm_elapsed = now - cia->ta_arm_last;
    cia->ta_arm_last = now;

    uint64_t cia_ticks = arm_to_cia_ticks(arm_elapsed);
    bool continuous = !(cia->cra & CIA_CRA_RUNMODE);

    int underflows = timer_advance(&cia->ta_counter, cia->ta_latch,
                                   continuous, cia_ticks);
    if (underflows > 0) {
        cia->icr_data |= CIA_ICR_TA;
        if (cia->cra & CIA_CRA_RUNMODE) {
            cia->cra &= (uint8_t)~CIA_CRA_START; // one-shot: stop
        }
    }
}

// ---------------------------------------------------------------------------
// Update Timer B based on elapsed real time
// (inmode 00 = phi2; modes 01/10/11 not implemented)
// ---------------------------------------------------------------------------

static void update_tb(CIA_State *cia)
{
    if (!(cia->crb & CIA_CRB_START))
        return;
    uint8_t inmode = (cia->crb >> 5) & 3;
    if (inmode != 0) return; // only phi2 mode

    uint64_t now = read_cntpct();
    uint64_t arm_elapsed = now - cia->tb_arm_last;
    cia->tb_arm_last = now;

    uint64_t cia_ticks = arm_to_cia_ticks(arm_elapsed);
    bool continuous = !(cia->crb & CIA_CRB_RUNMODE);

    int underflows = timer_advance(&cia->tb_counter, cia->tb_latch,
                                   continuous, cia_ticks);
    if (underflows > 0) {
        cia->icr_data |= CIA_ICR_TB;
        if (cia->crb & CIA_CRB_RUNMODE) {
            cia->crb &= (uint8_t)~CIA_CRB_START;
        }
    }
}

// ---------------------------------------------------------------------------
// Update TOD based on real time (Phase 2 fallback; replaced by cia_vbl_tick)
// ---------------------------------------------------------------------------

static void update_tod(CIA_State *cia)
{
    uint64_t now = read_cntpct();
    uint64_t per_tick = arm_per_tod_tick();
    if (per_tick == 0) return;

    uint64_t elapsed = now - cia->tod_arm_last;
    uint64_t ticks = elapsed / per_tick;

    if (ticks == 0) return;

    cia->tod_arm_last += ticks * per_tick;
    cia->tod = (cia->tod + (uint32_t)ticks) & 0x00FFFFFF;

    // Check TOD alarm
    if (cia->tod == (cia->tod_alarm & 0x00FFFFFF)) {
        cia->icr_data |= CIA_ICR_ALRM;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void cia_init(CIA_State *cia)
{
    uint64_t now = read_cntpct();
    cia->pra      = 0xFF;
    cia->prb      = 0xFF;
    cia->ddra     = 0x00; // all inputs by default
    cia->ddrb     = 0x00;
    cia->icr_mask = 0x00;
    cia->icr_data = 0x00;
    cia->cra      = 0x00;
    cia->crb      = 0x00;
    cia->ta_latch   = 0xFFFF;
    cia->ta_counter = 0xFFFF;
    cia->ta_arm_last = now;
    cia->tb_latch   = 0xFFFF;
    cia->tb_counter = 0xFFFF;
    cia->tb_arm_last = now;
    cia->tod        = 0;
    cia->tod_alarm  = 0;
    cia->tod_latch  = 0;
    cia->tod_latched = false;
    cia->tod_arm_last = now;
}

uint8_t cia_read(CIA_State *cia, int reg)
{
    switch (reg) {
    case CIA_REG_PRA:
        // Port A: input bits read back through DDR mask.
        // Output bits reflect PRA; input bits read as 1 (pulled high).
        return (cia->pra & cia->ddra) | (~cia->ddra & 0xFF);

    case CIA_REG_PRB:
        return (cia->prb & cia->ddrb) | (~cia->ddrb & 0xFF);

    case CIA_REG_DDRA:  return cia->ddra;
    case CIA_REG_DDRB:  return cia->ddrb;

    case CIA_REG_TALO:
        update_ta(cia);
        return (uint8_t)(cia->ta_counter & 0xFF);

    case CIA_REG_TAHI:
        update_ta(cia);
        return (uint8_t)(cia->ta_counter >> 8);

    case CIA_REG_TBLO:
        update_tb(cia);
        return (uint8_t)(cia->tb_counter & 0xFF);

    case CIA_REG_TBHI:
        update_tb(cia);
        return (uint8_t)(cia->tb_counter >> 8);

    case CIA_REG_TODLO:
        update_tod(cia);
        if (cia->tod_latched) {
            // Latch released on TODLO read (complete: lo/mid/hi read cycle done)
            cia->tod_latched = false;
            return (uint8_t)(cia->tod_latch & 0xFF);
        }
        return (uint8_t)(cia->tod & 0xFF);

    case CIA_REG_TODMID:
        if (cia->tod_latched)
            return (uint8_t)((cia->tod_latch >> 8) & 0xFF);
        update_tod(cia);
        return (uint8_t)((cia->tod >> 8) & 0xFF);

    case CIA_REG_TODHI:
        // Reading TODHI latches TOD (all three bytes frozen until TODLO read)
        update_tod(cia);
        cia->tod_latch   = cia->tod;
        cia->tod_latched = true;
        return (uint8_t)((cia->tod >> 16) & 0xFF);

    case CIA_REG_SDR:
        return 0xFF; // stub

    case CIA_REG_ICR: {
        // Update timers first so fresh flags are included
        update_ta(cia);
        update_tb(cia);
        update_tod(cia);
        uint8_t val = cia->icr_data;
        if (val & cia->icr_mask)
            val |= 0x80; // IR — any unmasked interrupt pending
        cia->icr_data = 0x00; // read-clear
        return val;
    }

    case CIA_REG_CRA:  return cia->cra;
    case CIA_REG_CRB:  return cia->crb;

    default: return 0xFF;
    }
}

void cia_write(CIA_State *cia, int reg, uint8_t val)
{
    switch (reg) {
    case CIA_REG_PRA:
        cia->pra = val;
        break;

    case CIA_REG_PRB:
        cia->prb = val;
        break;

    case CIA_REG_DDRA:
        cia->ddra = val;
        break;

    case CIA_REG_DDRB:
        cia->ddrb = val;
        break;

    case CIA_REG_TALO:
        // Write latches only, does not affect running counter
        cia->ta_latch = (cia->ta_latch & 0xFF00) | val;
        break;

    case CIA_REG_TAHI:
        cia->ta_latch = (cia->ta_latch & 0x00FF) | ((uint16_t)val << 8);
        // If timer is stopped, also load the counter (HRM: "if LOAD, load immediately")
        if (!(cia->cra & CIA_CRA_START)) {
            cia->ta_counter = cia->ta_latch;
            cia->ta_arm_last = read_cntpct();
        }
        break;

    case CIA_REG_TBLO:
        cia->tb_latch = (cia->tb_latch & 0xFF00) | val;
        break;

    case CIA_REG_TBHI:
        cia->tb_latch = (cia->tb_latch & 0x00FF) | ((uint16_t)val << 8);
        if (!(cia->crb & CIA_CRB_START)) {
            cia->tb_counter = cia->tb_latch;
            cia->tb_arm_last = read_cntpct();
        }
        break;

    case CIA_REG_TODLO:
        if (cia->crb & CIA_CRB_ALARM) {
            cia->tod_alarm = (cia->tod_alarm & 0x00FFFF00) | val;
        } else {
            cia->tod = (cia->tod & 0x00FFFF00) | val;
            // Writing TODLO re-enables the stopped TOD counter
            cia->tod_arm_last = read_cntpct();
        }
        break;

    case CIA_REG_TODMID:
        if (cia->crb & CIA_CRB_ALARM) {
            cia->tod_alarm = (cia->tod_alarm & 0x00FF00FF) | ((uint32_t)val << 8);
        } else {
            cia->tod = (cia->tod & 0x00FF00FF) | ((uint32_t)val << 8);
        }
        break;

    case CIA_REG_TODHI:
        if (cia->crb & CIA_CRB_ALARM) {
            cia->tod_alarm = (cia->tod_alarm & 0x0000FFFF) | ((uint32_t)val << 16);
        } else {
            // Writing TODHI stops TOD until TODLO is written
            cia->tod = (cia->tod & 0x0000FFFF) | ((uint32_t)val << 16);
        }
        break;

    case CIA_REG_SDR:
        // Stub: serial data register ignored
        break;

    case CIA_REG_ICR:
        // Bit 7: 1=set mask bits, 0=clear mask bits
        if (val & 0x80) {
            cia->icr_mask |= (val & 0x1F);
        } else {
            cia->icr_mask &= (uint8_t)~(val & 0x1F);
        }
        break;

    case CIA_REG_CRA: {
        uint8_t prev = cia->cra;
        cia->cra = val & ~CIA_CRA_LOAD; // LOAD bit is self-clearing

        // CRA_LOAD: immediately copy latch to counter
        if (val & CIA_CRA_LOAD) {
            cia->ta_counter = cia->ta_latch;
            cia->ta_arm_last = read_cntpct();
        }
        // Timer starting: sync ARM timestamp
        if (!(prev & CIA_CRA_START) && (val & CIA_CRA_START)) {
            cia->ta_arm_last = read_cntpct();
        }
        break;
    }

    case CIA_REG_CRB: {
        uint8_t prev = cia->crb;
        cia->crb = val & ~CIA_CRB_LOAD;

        if (val & CIA_CRB_LOAD) {
            cia->tb_counter = cia->tb_latch;
            cia->tb_arm_last = read_cntpct();
        }
        if (!(prev & CIA_CRB_START) && (val & CIA_CRB_START)) {
            cia->tb_arm_last = read_cntpct();
        }
        break;
    }

    default:
        break;
    }
}

int cia_irq_pending(CIA_State *cia)
{
    update_ta(cia);
    update_tb(cia);
    update_tod(cia);
    return (cia->icr_data & cia->icr_mask) != 0;
}

void cia_vbl_tick(CIA_State *cia)
{
    // Called at 50 Hz from the chipset loop (Phase 3+).
    // Advances TOD by one count, replacing the real-time fallback.
    cia->tod = (cia->tod + 1) & 0x00FFFFFF;
    if ((cia->tod & 0x00FFFFFF) == (cia->tod_alarm & 0x00FFFFFF)) {
        cia->icr_data |= CIA_ICR_ALRM;
    }
    cia->tod_arm_last = read_cntpct();
}
