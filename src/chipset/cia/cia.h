// src/chipset/cia/cia.h
//
// MOS 8520 CIA emulation.
// Reference: Amiga Hardware Reference Manual, 3rd Ed. Chapter 9.
// Behavioural reference: FS-UAE cia.cpp.

#ifndef _BELLATRIX_CIA_H
#define _BELLATRIX_CIA_H

#include <stdint.h>
#include <stdbool.h>

// ---- Register indices (bits A8-A11 of Amiga address) ----
#define CIA_REG_PRA    0
#define CIA_REG_PRB    1
#define CIA_REG_DDRA   2
#define CIA_REG_DDRB   3
#define CIA_REG_TALO   4   // Timer A counter low
#define CIA_REG_TAHI   5   // Timer A counter high
#define CIA_REG_TBLO   6
#define CIA_REG_TBHI   7
#define CIA_REG_TODLO  8   // TOD low (event counter)
#define CIA_REG_TODMID 9
#define CIA_REG_TODHI  10
#define CIA_REG_SDR    12  // Serial Data Register (stub)
#define CIA_REG_ICR    13  // Interrupt Control Register
#define CIA_REG_CRA    14  // Control Register A
#define CIA_REG_CRB    15  // Control Register B

// ---- ICR bits ----
#define CIA_ICR_TA   (1 << 0)  // Timer A underflow
#define CIA_ICR_TB   (1 << 1)  // Timer B underflow
#define CIA_ICR_ALRM (1 << 2)  // TOD alarm
#define CIA_ICR_SP   (1 << 3)  // Serial port
#define CIA_ICR_FLG  (1 << 4)  // FLAG line
// Bit 7 (read): IR — any unmasked interrupt pending

// ---- CRA bits ----
#define CIA_CRA_START   (1 << 0)  // Timer A running
#define CIA_CRA_PBON    (1 << 1)  // Timer A output on PB6
#define CIA_CRA_OUTMODE (1 << 2)  // 0=pulse, 1=toggle on PB6
#define CIA_CRA_RUNMODE (1 << 3)  // 0=continuous, 1=one-shot
#define CIA_CRA_LOAD    (1 << 4)  // Force-load timer from latch (self-clearing)
#define CIA_CRA_INMODE  (1 << 5)  // 0=phi2, 1=CNT pin
#define CIA_CRA_SPMODE  (1 << 6)  // Serial port mode
#define CIA_CRA_TODIN   (1 << 7)  // TOD clock: 0=50 Hz, 1=60 Hz

// ---- CRB bits ----
#define CIA_CRB_START    (1 << 0)
#define CIA_CRB_PBON     (1 << 1)
#define CIA_CRB_OUTMODE  (1 << 2)
#define CIA_CRB_RUNMODE  (1 << 3)  // 0=continuous, 1=one-shot
#define CIA_CRB_LOAD     (1 << 4)
#define CIA_CRB_INMODE0  (1 << 5)  // 00=phi2, 01=CNT, 10=TA undf, 11=TA undf+CNT
#define CIA_CRB_INMODE1  (1 << 6)
#define CIA_CRB_ALARM    (1 << 7)  // 0=write TOD, 1=write alarm

// ---- CIA clock (PAL) ----
#define CIA_FREQ_PAL_HZ  709379ULL   // phi2 clock, ~0.709 MHz

// ---- State ----
typedef struct {
    // Port registers
    uint8_t  pra, prb;
    uint8_t  ddra, ddrb;

    // Interrupt control
    uint8_t  icr_mask;      // Interrupt mask (set/cleared by ICR writes)
    uint8_t  icr_data;      // Interrupt flags (cleared on read)

    // Control
    uint8_t  cra, crb;

    // Timer A
    uint16_t ta_latch;      // Reload value written by software
    uint16_t ta_counter;    // Running countdown
    uint64_t ta_arm_last;   // ARM CNTPCT when ta_counter was last updated

    // Timer B
    uint16_t tb_latch;
    uint16_t tb_counter;
    uint64_t tb_arm_last;

    // Time of Day
    uint32_t tod;           // 24-bit BCD counter (in binary here; Phase 3 adds VBL tick)
    uint32_t tod_alarm;
    uint32_t tod_latch;     // Latched value (set on TODHI read)
    bool     tod_latched;   // True until TODLO is read
    uint64_t tod_arm_last;  // ARM CNTPCT when tod was last incremented
} CIA_State;

// ---- Public API ----

// Initialise to power-on state.
void cia_init(CIA_State *cia);

// Read a register (0-15). Called from bellatrix_bus_access.
uint8_t cia_read(CIA_State *cia, int reg);

// Write a register (0-15). Called from bellatrix_bus_access.
void cia_write(CIA_State *cia, int reg, uint8_t val);

// Returns non-zero if any unmasked interrupt is pending.
int cia_irq_pending(CIA_State *cia);

// Advance TOD by one tick (called from chipset_loop_tick at VBL rate, Phase 3+).
void cia_vbl_tick(CIA_State *cia);

#endif /* _BELLATRIX_CIA_H */
