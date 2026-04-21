#pragma once

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* RTC model                                                                 */
/* ------------------------------------------------------------------------- */

typedef enum RTCModel
{
    RTC_MODEL_NONE  = 0,
    RTC_MODEL_OKI   = 1,
    RTC_MODEL_RICOH = 2
} RTCModel;

/* ------------------------------------------------------------------------- */
/* RTC state                                                                 */
/* ------------------------------------------------------------------------- */

typedef struct RTCState
{
    /*
     * Active RTC model.
     */
    RTCModel model;

    /*
     * Raw RTC registers.
     *
     * We keep 4 banks of 16 nibbles to leave room for future refinement.
     * For the current Bellatrix stage, bank 0 is the important one.
     */
    uint8_t reg[4][16];

    /*
     * Offset from host wall clock.
     *
     * emulated_time = host_time + time_offset
     */
    int64_t time_offset;

} RTCState;

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void rtc_init(RTCState *rtc, RTCModel model);
void rtc_reset(RTCState *rtc);
void rtc_set_model(RTCState *rtc, RTCModel model);
RTCModel rtc_get_model(const RTCState *rtc);

/* ------------------------------------------------------------------------- */
/* time management                                                           */
/* ------------------------------------------------------------------------- */

time_t rtc_get_time(RTCState *rtc);
void rtc_set_time(RTCState *rtc, time_t t);
void rtc_update(RTCState *rtc);

/* ------------------------------------------------------------------------- */
/* register access                                                           */
/* ------------------------------------------------------------------------- */

uint8_t rtc_read_reg(RTCState *rtc, uint8_t nr);
void rtc_write_reg(RTCState *rtc, uint8_t nr, uint8_t value);

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
/* ------------------------------------------------------------------------- */

uint8_t rtc_current_bank(const RTCState *rtc);

#ifdef __cplusplus
}
#endif