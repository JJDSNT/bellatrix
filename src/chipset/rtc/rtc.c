#include "rtc.h"

#include <assert.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* internal helpers                                                          */
/* ------------------------------------------------------------------------- */

static inline uint8_t rtc_nibble(uint8_t v)
{
    return (uint8_t)(v & 0x0F);
}

static inline uint8_t rtc_bcd_lo(int v)
{
    return (uint8_t)(v % 10);
}

static inline uint8_t rtc_bcd_hi(int v)
{
    return (uint8_t)(v / 10);
}

static inline int rtc_from_bcd(uint8_t lo, uint8_t hi)
{
    return (int)(rtc_nibble(lo) + 10 * rtc_nibble(hi));
}

static void rtc_localtime_copy(time_t t, struct tm *out)
{
#if defined(_WIN32)
    localtime_s(out, &t);
#elif defined(_POSIX_VERSION)
    localtime_r(&t, out);
#else
    struct tm *tmp = localtime(&t);
    if (tmp) {
        *out = *tmp;
    } else {
        memset(out, 0, sizeof(*out));
    }
#endif
}

static uint8_t rtc_bank_internal(const RTCState *rtc)
{
    /*
     * Simplified bank selection:
     *
     * We use the low 2 bits of register D as bank selector.
     * This is a pragmatic Bellatrix choice for now: it preserves banked state
     * and is easy to refine later if stricter chip behavior becomes necessary.
     */
    return (uint8_t)(rtc->reg[0][0xD] & 0x03);
}

uint8_t rtc_current_bank(const RTCState *rtc)
{
    return rtc_bank_internal(rtc);
}

static void rtc_apply_model_defaults(RTCState *rtc)
{
    memset(rtc->reg, 0, sizeof(rtc->reg));

    switch (rtc->model) {

        case RTC_MODEL_OKI:
            rtc->reg[0][0xD] = 0x1;
            rtc->reg[0][0xE] = 0x0;
            rtc->reg[0][0xF] = 0x4;
            break;

        case RTC_MODEL_RICOH:
            rtc->reg[0][0xD] = 0x8;
            rtc->reg[0][0xE] = 0x0;
            rtc->reg[0][0xF] = 0x0;
            break;

        case RTC_MODEL_NONE:
        default:
            break;
    }
}

static void rtc_time_to_regs_oki(RTCState *rtc, const struct tm *t)
{
    int hour = t->tm_hour;
    int pm = 0;
    int is_24h = (rtc->reg[0][0xF] & 0x4) ? 1 : 0;

    rtc->reg[0][0x0] = rtc_bcd_lo(t->tm_sec);
    rtc->reg[0][0x1] = rtc_bcd_hi(t->tm_sec);

    rtc->reg[0][0x2] = rtc_bcd_lo(t->tm_min);
    rtc->reg[0][0x3] = rtc_bcd_hi(t->tm_min);

    if (!is_24h) {
        if (hour == 0) {
            hour = 12;
            pm = 0;
        } else if (hour == 12) {
            pm = 1;
        } else if (hour > 12) {
            hour -= 12;
            pm = 1;
        }
    }

    rtc->reg[0][0x4] = rtc_bcd_lo(hour);
    rtc->reg[0][0x5] = rtc_bcd_hi(hour);
    if (!is_24h && pm)
        rtc->reg[0][0x5] |= 0x4;

    rtc->reg[0][0x6] = rtc_bcd_lo(t->tm_mday);
    rtc->reg[0][0x7] = rtc_bcd_hi(t->tm_mday);

    rtc->reg[0][0x8] = rtc_bcd_lo(t->tm_mon + 1);
    rtc->reg[0][0x9] = rtc_bcd_hi(t->tm_mon + 1);

    rtc->reg[0][0xA] = rtc_bcd_lo(t->tm_year % 100);
    rtc->reg[0][0xB] = rtc_bcd_hi(t->tm_year % 100);

    rtc->reg[0][0xC] = (uint8_t)(t->tm_wday & 0x0F);
}

static void rtc_time_to_regs_ricoh(RTCState *rtc, const struct tm *t)
{
    int hour = t->tm_hour;
    int pm = 0;
    int is_24h = (rtc->reg[0][0xA] & 0x1) ? 1 : 0;

    rtc->reg[0][0x0] = rtc_bcd_lo(t->tm_sec);
    rtc->reg[0][0x1] = rtc_bcd_hi(t->tm_sec);

    rtc->reg[0][0x2] = rtc_bcd_lo(t->tm_min);
    rtc->reg[0][0x3] = rtc_bcd_hi(t->tm_min);

    if (!is_24h) {
        if (hour == 0) {
            hour = 12;
            pm = 0;
        } else if (hour == 12) {
            pm = 1;
        } else if (hour > 12) {
            hour -= 12;
            pm = 1;
        }
    }

    rtc->reg[0][0x4] = rtc_bcd_lo(hour);
    rtc->reg[0][0x5] = rtc_bcd_hi(hour);
    if (!is_24h && pm)
        rtc->reg[0][0x5] |= 0x2;

    /*
     * Ricoh layout differs from OKI.
     */
    rtc->reg[0][0x6] = (uint8_t)((t->tm_yday / 7) & 0x0F);
    rtc->reg[0][0x7] = rtc_bcd_lo(t->tm_mday);
    rtc->reg[0][0x8] = rtc_bcd_hi(t->tm_mday);
    rtc->reg[0][0x9] = rtc_bcd_lo(t->tm_mon + 1);
    rtc->reg[0][0xA] = (uint8_t)((rtc->reg[0][0xA] & 0x1) | (rtc_bcd_hi(t->tm_mon + 1) << 1));
    rtc->reg[0][0xB] = rtc_bcd_lo(t->tm_year % 100);
    rtc->reg[0][0xC] = rtc_bcd_hi(t->tm_year % 100);

    /*
     * Keep only the 24h bit in 0xA plus the month high digit encoding above.
     */
    rtc->reg[0][0xA] &= 0x03;

    /*
     * Trim unused bits in alarm bank as a mild nod to the Ricoh layout.
     */
    rtc->reg[1][0x0] &= 0x0;
    rtc->reg[1][0x1] &= 0x0;
    rtc->reg[1][0x2] &= 0xF;
    rtc->reg[1][0x3] &= 0x7;
    rtc->reg[1][0x4] &= 0xF;
    rtc->reg[1][0x5] &= 0x3;
    rtc->reg[1][0x6] &= 0x7;
    rtc->reg[1][0x7] &= 0xF;
    rtc->reg[1][0x8] &= 0x3;
    rtc->reg[1][0x9] &= 0x0;
    rtc->reg[1][0xA] &= 0x1;
    rtc->reg[1][0xB] &= 0x3;
    rtc->reg[1][0xC] &= 0x0;
}

static void rtc_time_to_regs(RTCState *rtc)
{
    time_t now;
    struct tm t;

    if (rtc->model == RTC_MODEL_NONE)
        return;

    now = rtc_get_time(rtc);
    rtc_localtime_copy(now, &t);

    switch (rtc->model) {

        case RTC_MODEL_OKI:
            rtc_time_to_regs_oki(rtc, &t);
            break;

        case RTC_MODEL_RICOH:
            rtc_time_to_regs_ricoh(rtc, &t);
            break;

        case RTC_MODEL_NONE:
        default:
            break;
    }
}

static void rtc_regs_to_time_oki(RTCState *rtc, struct tm *t)
{
    int hour = rtc_from_bcd(rtc->reg[0][0x4], rtc->reg[0][0x5] & 0x3);
    int is_24h = (rtc->reg[0][0xF] & 0x4) ? 1 : 0;
    int pm = (rtc->reg[0][0x5] & 0x4) ? 1 : 0;

    memset(t, 0, sizeof(*t));

    t->tm_sec  = rtc_from_bcd(rtc->reg[0][0x0], rtc->reg[0][0x1]);
    t->tm_min  = rtc_from_bcd(rtc->reg[0][0x2], rtc->reg[0][0x3]);

    if (!is_24h) {
        if (hour == 12) {
            hour = pm ? 12 : 0;
        } else if (pm) {
            hour += 12;
        }
    }
    t->tm_hour = hour;

    t->tm_mday = rtc_from_bcd(rtc->reg[0][0x6], rtc->reg[0][0x7]);
    t->tm_mon  = rtc_from_bcd(rtc->reg[0][0x8], rtc->reg[0][0x9]) - 1;
    t->tm_year = rtc_from_bcd(rtc->reg[0][0xA], rtc->reg[0][0xB]);

    /*
     * Interpret 00..69 as 2000..2069 and 70..99 as 1970..1999.
     * mktime expects years since 1900.
     */
    t->tm_year += (t->tm_year < 70) ? 100 : 0;
}

static void rtc_regs_to_time_ricoh(RTCState *rtc, struct tm *t)
{
    int hour = rtc_from_bcd(rtc->reg[0][0x4], rtc->reg[0][0x5] & 0x1);
    int is_24h = (rtc->reg[0][0xA] & 0x1) ? 1 : 0;
    int pm = (rtc->reg[0][0x5] & 0x2) ? 1 : 0;
    int mon_hi = (rtc->reg[0][0xA] >> 1) & 0x1;

    memset(t, 0, sizeof(*t));

    t->tm_sec  = rtc_from_bcd(rtc->reg[0][0x0], rtc->reg[0][0x1]);
    t->tm_min  = rtc_from_bcd(rtc->reg[0][0x2], rtc->reg[0][0x3]);

    if (!is_24h) {
        if (hour == 12) {
            hour = pm ? 12 : 0;
        } else if (pm) {
            hour += 12;
        }
    }
    t->tm_hour = hour;

    t->tm_mday = rtc_from_bcd(rtc->reg[0][0x7], rtc->reg[0][0x8]);
    t->tm_mon  = (rtc_nibble(rtc->reg[0][0x9]) + 10 * mon_hi) - 1;
    t->tm_year = rtc_from_bcd(rtc->reg[0][0xB], rtc->reg[0][0xC]);

    t->tm_year += (t->tm_year < 70) ? 100 : 0;
}

static void rtc_regs_to_time(RTCState *rtc)
{
    struct tm t;
    time_t new_time;

    if (rtc->model == RTC_MODEL_NONE)
        return;

    switch (rtc->model) {

        case RTC_MODEL_OKI:
            rtc_regs_to_time_oki(rtc, &t);
            break;

        case RTC_MODEL_RICOH:
            rtc_regs_to_time_ricoh(rtc, &t);
            break;

        case RTC_MODEL_NONE:
        default:
            return;
    }

    new_time = mktime(&t);
    if (new_time != (time_t)-1)
        rtc_set_time(rtc, new_time);
}

static uint8_t rtc_read_special(const RTCState *rtc, uint8_t nr)
{
    return rtc_nibble(rtc->reg[0][nr]);
}

static void rtc_write_special(RTCState *rtc, uint8_t nr, uint8_t value)
{
    rtc->reg[0][nr] = rtc_nibble(value);
}

/* ------------------------------------------------------------------------- */
/* lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void rtc_init(RTCState *rtc, RTCModel model)
{
    memset(rtc, 0, sizeof(*rtc));
    rtc->model = model;
    rtc_apply_model_defaults(rtc);
    rtc_update(rtc);
}

void rtc_reset(RTCState *rtc)
{
    RTCModel model = rtc->model;
    int64_t offset = rtc->time_offset;

    memset(rtc, 0, sizeof(*rtc));
    rtc->model = model;
    rtc->time_offset = offset;

    rtc_apply_model_defaults(rtc);
    rtc_update(rtc);
}

void rtc_set_model(RTCState *rtc, RTCModel model)
{
    rtc->model = model;
    rtc_apply_model_defaults(rtc);
    rtc_update(rtc);
}

RTCModel rtc_get_model(const RTCState *rtc)
{
    return rtc->model;
}

/* ------------------------------------------------------------------------- */
/* time management                                                           */
/* ------------------------------------------------------------------------- */

time_t rtc_get_time(RTCState *rtc)
{
    time_t host_now = time(NULL);

    if (rtc->model == RTC_MODEL_NONE)
        return host_now;

    return (time_t)((int64_t)host_now + rtc->time_offset);
}

void rtc_set_time(RTCState *rtc, time_t t)
{
    time_t host_now = time(NULL);
    rtc->time_offset = (int64_t)t - (int64_t)host_now;
}

void rtc_update(RTCState *rtc)
{
    rtc_time_to_regs(rtc);
}

/* ------------------------------------------------------------------------- */
/* register access                                                           */
/* ------------------------------------------------------------------------- */

uint8_t rtc_read_reg(RTCState *rtc, uint8_t nr)
{
    uint8_t bank;

    assert(nr < 16);

    if (rtc->model == RTC_MODEL_NONE)
        return 0x0;

    rtc_update(rtc);

    switch (nr) {

        case 0xD:
        case 0xE:
        case 0xF:
            return rtc_read_special(rtc, nr);

        default:
            bank = rtc_bank_internal(rtc);
            return rtc_nibble(rtc->reg[bank][nr]);
    }
}

void rtc_write_reg(RTCState *rtc, uint8_t nr, uint8_t value)
{
    uint8_t bank;
    uint8_t nibble;

    assert(nr < 16);

    if (rtc->model == RTC_MODEL_NONE)
        return;

    nibble = rtc_nibble(value);

    switch (nr) {

        case 0xD:
        case 0xE:
        case 0xF:
            rtc_write_special(rtc, nr, nibble);
            return;

        default:
            rtc_update(rtc);
            bank = rtc_bank_internal(rtc);
            rtc->reg[bank][nr] = nibble;

            /*
             * Only bank 0 is interpreted as the live clock image.
             * Other banks are preserved as raw RTC state.
             */
            if (bank == 0)
                rtc_regs_to_time(rtc);
            return;
    }
}