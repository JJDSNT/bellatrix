// src/host/raspi3/posix_time.c
//
// Bare-metal stubs for POSIX time functions used by rtc.c.
// time() returns seconds elapsed since boot (epoch offset = 0).
// localtime_r / mktime implement simple UTC math (no TZ, no DST).

#include <time.h>
#include <stdint.h>

#include "raspi3/time.h"
#include "support.h"

/* ---------------------------------------------------------------------------
 * time()
 * Returns seconds since an arbitrary epoch (boot).
 * For the current Bellatrix stage this is sufficient; the RTC stores its
 * own time_offset so absolute wall-clock accuracy is a later concern.
 * ------------------------------------------------------------------------- */

time_t time(time_t *out)
{
    uint64_t freq = raspi3_counter_freq();
    time_t t = 0;

    if (freq)
        t = (time_t)(raspi3_counter_get() / freq);

    if (out)
        *out = t;
    return t;
}

/* ---------------------------------------------------------------------------
 * Simple UTC calendar math — no DST, no leap seconds, Gregorian proleptic.
 * ------------------------------------------------------------------------- */

static int is_leap(int y)
{
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

static const int days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
};

struct tm *localtime_r(const time_t *timer, struct tm *result)
{
    time_t t = *timer;
    int sec, min, hour, day, mon, year, wday, yday;

    sec  = (int)(t % 60); t /= 60;
    min  = (int)(t % 60); t /= 60;
    hour = (int)(t % 24); t /= 24;

    /* Days since 1970-01-01 (t now) */
    wday = (int)((t + 4) % 7); /* 1970-01-01 was Thursday = 4 */

    year = 1970;
    while (1) {
        int days_in_year = is_leap(year) ? 366 : 365;
        if (t < (time_t)days_in_year) break;
        t -= days_in_year;
        year++;
    }
    yday = (int)t;

    mon = 0;
    while (mon < 11) {
        int dim = days_before_month[mon + 1] - days_before_month[mon];
        if (mon == 1 && is_leap(year)) dim++;
        if (t < (time_t)dim) break;
        t -= dim;
        mon++;
    }
    day = (int)t + 1;

    result->tm_sec   = sec;
    result->tm_min   = min;
    result->tm_hour  = hour;
    result->tm_mday  = day;
    result->tm_mon   = mon;
    result->tm_year  = year - 1900;
    result->tm_wday  = wday;
    result->tm_yday  = yday;
    result->tm_isdst = 0;

    return result;
}

struct tm *localtime(const time_t *timer)
{
    static struct tm s_tm;
    return localtime_r(timer, &s_tm);
}

time_t mktime(struct tm *t)
{
    int y = t->tm_year + 1900;
    int m = t->tm_mon;      /* 0..11 */
    int d = t->tm_mday - 1; /* 0-based */
    time_t days;
    int i;

    /* Days from 1970 to start of year */
    days = 0;
    for (i = 1970; i < y; i++)
        days += is_leap(i) ? 366 : 365;

    /* Days in completed months of this year */
    days += days_before_month[m];
    if (m > 1 && is_leap(y))
        days++;

    days += d;

    return days * 86400
         + (time_t)t->tm_hour * 3600
         + (time_t)t->tm_min  * 60
         + (time_t)t->tm_sec;
}

/* ---------------------------------------------------------------------------
 * assert support
 * ------------------------------------------------------------------------- */

void __assert_fail(const char *expr, const char *file,
                   unsigned int line, const char *func)
{
    kprintf("[ASSERT] %s:%u %s: %s\n", file, line, func, expr);
    for (;;) asm volatile("wfe");
}
